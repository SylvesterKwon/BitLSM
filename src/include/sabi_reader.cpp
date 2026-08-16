#include <malloc.h>  // malloc_usable_size
#include <sabi.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <type_traits>

#include "cache/cache_key.h"
#include "rocksdb/cache.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/coding_lean.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace {

// Returns true only when `cond` is provably unsatisfiable against the SST
// described by `bm` and `schema`. Returns false if uncertain or unsupported.
bool ConditionImpossible(const bit_lsm::SABICondition& cond,
                         const bit_lsm::SABISchema& schema,
                         const bit_lsm::BitmapIndex& bm) {
  uint32_t idx = cond.attr_idx;
  if (idx >= bm.binning_policy.size()) return false;
  if (idx >= schema.attr_num()) return false;

  if (schema.roles[idx] == bit_lsm::AttrRole::ORDERED) {
    if (!std::holds_alternative<std::vector<uint64_t>>(bm.binning_policy[idx]))
      return false;
    const auto& bounds =
        std::get<std::vector<uint64_t>>(bm.binning_policy[idx]);
    if (bounds.size() < 2) return false;
    uint64_t mn = bounds.front(), mx = bounds.back();
    uint64_t val = cond.okey;

    switch (cond.op) {
      case bit_lsm::CompareOp::GREATER:
        return val >= mx;
      case bit_lsm::CompareOp::GREATER_EQUAL:
        return val > mx;
      case bit_lsm::CompareOp::LESS:
        return val <= mn;
      case bit_lsm::CompareOp::LESS_EQUAL:
        return val < mn;
      case bit_lsm::CompareOp::EQUAL:
        return val < mn || val > mx;
    }
  } else if (schema.roles[idx] == bit_lsm::AttrRole::UNORDERED) {
    if (cond.op != bit_lsm::CompareOp::EQUAL) return false;
    if (!std::holds_alternative<std::vector<std::pair<std::string, uint32_t>>>(
            bm.binning_policy[idx]))
      return false;
    const auto& entries =
        std::get<std::vector<std::pair<std::string, uint32_t>>>(
            bm.binning_policy[idx]);
    auto it =
        std::lower_bound(entries.begin(), entries.end(), cond.bytes,
                         [](const std::pair<std::string, uint32_t>& e,
                            const std::string& v) { return e.first < v; });
    return !(it != entries.end() && it->first == cond.bytes);
  }
  return false;
}

void DeleteSABICachedBin(rocksdb::Cache::ObjectPtr obj,
                         rocksdb::MemoryAllocator* /*allocator*/) {
  delete static_cast<bit_lsm::SABICachedBin*>(obj);
}

const rocksdb::Cache::CacheItemHelper* SABICachedBinHelper() {
  static const rocksdb::Cache::CacheItemHelper helper(
      rocksdb::CacheEntryRole::kIndexBlock, &DeleteSABICachedBin);
  return &helper;
}

}  // namespace

namespace bit_lsm {

namespace {
// Process-wide totals for the on-demand bin cache (see block_prefetch_queue.
// cpp for the same pattern): these reads bypass BlockBasedTable, so RocksDB's
// tickers never see them.
std::atomic<uint64_t> g_bin_hits{0};
std::atomic<uint64_t> g_bin_misses{0};
std::atomic<uint64_t> g_bytes_read{0};
std::atomic<uint64_t> g_bitmaps_loaded{0};
}  // namespace

SABIBinCacheStats GetSABIBinCacheStats() {
  return {g_bin_hits.load(memory_order_relaxed),
          g_bin_misses.load(memory_order_relaxed),
          g_bytes_read.load(memory_order_relaxed),
          g_bitmaps_loaded.load(memory_order_relaxed)};
}

void ResetSABIBinCacheStats() {
  g_bin_hits.store(0, memory_order_relaxed);
  g_bin_misses.store(0, memory_order_relaxed);
  g_bytes_read.store(0, memory_order_relaxed);
  g_bitmaps_loaded.store(0, memory_order_relaxed);
}

// ========================================================================
// SABIUDIIterator Implementation
// ========================================================================

SABIUDIIterator::SABIUDIIterator(const SABIReader* reader) {}

void SABIUDIIterator::Prepare(const ScanOptions scan_opts[], size_t num_opts) {
};

Status SABIUDIIterator::SeekAndGetResult(const Slice& target,
                                         IterateResult* result) {
  return Status::OK();
};

Status SABIUDIIterator::NextAndGetResult(IterateResult* result) {
  return Status::OK();
};

UserDefinedIndexBuilder::BlockHandle SABIUDIIterator::value() {
  return UserDefinedIndexBuilder::BlockHandle{};
};

// ========================================================================
// SABIReader Implementation
// ========================================================================

SABIReader::SABIReader(Slice& index_block)
    : SABIReader(index_block, SABIReaderMode::kResident) {}

SABIReader::SABIReader(Slice& index_block, SABIReaderMode mode) : mode_(mode) {
  // 1. Read footer: [directory_off u32][version u32][magic u32]
  // (magic/version/directory bounds already validated by
  // SABIFactory::NewReader)
  assert(index_block.size() >= 3 * sizeof(uint32_t) &&
         DecodeFixed32(index_block.data() + index_block.size() -
                       sizeof(uint32_t)) == kSABIFooterMagic);
  const uint32_t version = DecodeFixed32(
      index_block.data() + index_block.size() - 2 * sizeof(uint32_t));
  uint32_t directory_off = DecodeFixed32(
      index_block.data() + index_block.size() - 3 * sizeof(uint32_t));

  // 2. Read directory forward: attr_num first, so every following array's
  // size is known. Roles reconstruct the schema residue without any binding.
  const char* dir = index_block.data() + directory_off;
  uint32_t attr_num = DecodeFixed32(dir);
  dir += sizeof(uint32_t);
  schema_.roles.resize(attr_num);
  for (uint32_t i = 0; i < attr_num; ++i)
    schema_.roles[i] = static_cast<AttrRole>(static_cast<uint8_t>(dir[i]));
  dir += attr_num;
  bitmap_index.bitmap_nums.resize(attr_num);
  uint32_t total_bins = 0;
  for (uint32_t i = 0; i < attr_num; ++i) {
    bitmap_index.bitmap_nums[i] = DecodeFixed32(dir);
    total_bins += bitmap_index.bitmap_nums[i];
    dir += sizeof(uint32_t);
  }
  uint32_t index_entries_cnt_ = DecodeFixed32(dir);
  dir += sizeof(uint32_t);
  // v6+: per-attr exact distinct counts. A v5 blob has no such array; leave
  // zeros (= unknown, estimator floor disabled for this SST).
  distinct_cnts.assign(attr_num, 0);
  if (version >= 6) {
    for (uint32_t i = 0; i < attr_num; ++i) {
      distinct_cnts[i] = DecodeFixed64(dir);
      dir += sizeof(uint64_t);
    }
  }
  // v7+: per-bin cardinalities (tombstone last) and exact frozen sizes. A
  // v5/v6 blob has neither; bin_cardinalities stays empty (= unknown, derive
  // from decoded bitmaps) and bitmap_sizes_ is derived below from offsets.
  bin_cardinalities.clear();
  if (version >= 7) {
    bin_cardinalities.resize(total_bins + 1);
    for (uint32_t i = 0; i <= total_bins; ++i) {
      bin_cardinalities[i] = DecodeFixed32(dir);
      dir += sizeof(uint32_t);
    }
    bitmap_sizes_.resize(total_bins + 1);
    for (uint32_t i = 0; i <= total_bins; ++i) {
      bitmap_sizes_[i] = DecodeFixed32(dir);
      dir += sizeof(uint32_t);
    }
  }
  vector<uint32_t> policy_offsets(attr_num + 1);
  for (uint32_t i = 0; i <= attr_num; ++i) {
    policy_offsets[i] = DecodeFixed32(dir);
    dir += sizeof(uint32_t);
  }
  uint32_t bitmaps_cnt = total_bins + 1;  // + tombstone bitmap
  bitmap_offsets_.resize(bitmaps_cnt + 1);
  for (uint32_t i = 0; i <= bitmaps_cnt; ++i) {
    bitmap_offsets_[i] = DecodeFixed32(dir);
    dir += sizeof(uint32_t);
  }
  if (version < 7) {
    // Pre-padding blobs: sizes are exactly the offset differences.
    bitmap_sizes_.resize(bitmaps_cnt);
    for (uint32_t i = 0; i < bitmaps_cnt; ++i)
      bitmap_sizes_[i] = bitmap_offsets_[i + 1] - bitmap_offsets_[i];
  }

  data_entries_cnt_psum.resize(index_entries_cnt_);
  block_handles.resize(index_entries_cnt_);

  // 3. Read binning policies. ORDERED bodies are headerless (bin_count+1
  // boundaries); UNORDERED bodies carry their entry count.
  bitmap_index.binning_policy.resize(attr_num);
  for (uint32_t i = 0; i < attr_num; ++i) {
    const char* ptr = index_block.data() + policy_offsets[i];

    if (schema_.roles[i] == AttrRole::UNORDERED) {
      uint32_t cur_binning_policy_entry_count = DecodeFixed32(ptr);
      ptr += sizeof(uint32_t);
      // read {length prefixed string + uint32t (bin number)}
      vector<pair<string, uint32_t>> cur_binning_policy(
          cur_binning_policy_entry_count);
      for (uint32_t j = 0; j < cur_binning_policy_entry_count; ++j) {
        uint32_t key_len = 0;
        // requires at least 5 bytes
        const char* key_start = GetVarint32Ptr(ptr, ptr + 5, &key_len);
        string key(key_start, key_len);
        ptr = key_start + key_len;
        cur_binning_policy[j] = {key, DecodeFixed32(ptr)};
        ptr += sizeof(uint32_t);
      }
      bitmap_index.binning_policy[i] = std::move(cur_binning_policy);
    } else if (schema_.roles[i] == AttrRole::ORDERED) {
      uint32_t boundary_count = bitmap_index.bitmap_nums[i] + 1;
      vector<uint64_t> cur_binning_policy(boundary_count);
      for (uint32_t j = 0; j < boundary_count; ++j)
        cur_binning_policy[j] = DecodeFixed64(ptr + j * sizeof(uint64_t));
      bitmap_index.binning_policy[i] = std::move(cur_binning_policy);
    } else {
      assert(false);
    }
  }

  // 4. Materialize frozen bitmap views. Metadata mode skips this entirely:
  // bitmap_index.bitmaps and tombstone_bitmap stay default-constructed
  // (empty), and no aligned copy of the bitmap region is made. Everything a
  // metadata-only reader needs (bitmap_offsets_, bitmap_sizes_,
  // bin_cardinalities, binning policies, psum, handles) is already copied
  // out above and below, so nothing references index_block afterwards --
  // which is what RetainsIndexContents() == false promises.
  if (mode_ == SABIReaderMode::kResident) {
    bitmap_index.bitmaps.resize(bitmaps_cnt - 1);  // tombstone kept separately
    if (version >= 7) {
      // v7 starts are 32B-aligned blob-relative, so one 32B-aligned copy of
      // the whole region backs every view zero-copy: N allocs+copies -> 1.
      const uint32_t region_off = bitmap_offsets_[0];
      const uint32_t region_len = bitmap_offsets_[bitmaps_cnt] - region_off;
      void* arena = nullptr;
      if (posix_memalign(&arena, 32, region_len == 0 ? 32 : region_len) != 0) {
        // No status channel here, and a silently-empty bin would under-return
        // rows; treat allocation failure as fatal rather than degrade quietly.
        std::abort();
      }
      AlignedPtr owned(static_cast<char*>(arena), std::free);
      memcpy(owned.get(), index_block.data() + region_off, region_len);
      for (uint32_t i = 0; i < bitmaps_cnt; ++i) {
        const char* p = owned.get() + (bitmap_offsets_[i] - region_off);
        if (i < bitmaps_cnt - 1) {
          bitmap_index.bitmaps[i] = Roaring::frozenView(p, bitmap_sizes_[i]);
        } else {
          bitmap_index.tombstone_bitmap =
              Roaring::frozenView(p, bitmap_sizes_[i]);
        }
      }
      managed_buffers_.push_back(std::move(owned));
    } else {
      for (uint32_t i = 0; i < bitmaps_cnt; ++i) {
        const uint32_t size = bitmap_sizes_[i];
        const char* raw_ptr = index_block.data() + bitmap_offsets_[i];
        void* aligned_ptr = nullptr;
        if (posix_memalign(&aligned_ptr, 32, size == 0 ? 32 : size) != 0) {
          // No status channel here, and a silently-empty bin would under-return
          // rows; treat allocation failure as fatal rather than degrade
          // quietly.
          std::abort();
        }
        AlignedPtr managed_aligned_ptr(static_cast<char*>(aligned_ptr),
                                       std::free);
        memcpy(managed_aligned_ptr.get(), raw_ptr, size);
        if (i < bitmaps_cnt - 1) {
          bitmap_index.bitmaps[i] =
              Roaring::frozenView(managed_aligned_ptr.get(), size);
        } else {
          bitmap_index.tombstone_bitmap =
              Roaring::frozenView(managed_aligned_ptr.get(), size);
        }
        managed_buffers_.push_back(std::move(managed_aligned_ptr));
      }
    }
  }

  // 5. Read index block related information
  for (uint32_t i = 0; i < index_entries_cnt_; ++i) {
    const char* cur_index_entry_base_ptr =
        index_block.data() + i * 3 * sizeof(uint32_t);

    data_entries_cnt_psum[i] = DecodeFixed32(cur_index_entry_base_ptr);
    block_handles[i].set_offset(
        DecodeFixed32(cur_index_entry_base_ptr + sizeof(uint32_t)));
    block_handles[i].set_size(
        DecodeFixed32(cur_index_entry_base_ptr + 2 * sizeof(uint32_t)));
  }

  // Dump();
}

unique_ptr<UserDefinedIndexIterator> SABIReader::NewIterator(
    const ReadOptions& read_options) {
  return make_unique<SABIUDIIterator>(this);
};

uint32_t SABIReader::TotalBins() const {
  return bitmap_offsets_.empty()
             ? 0
             : static_cast<uint32_t>(bitmap_offsets_.size() - 2);
}

uint64_t SABIReader::BinCardinality(uint32_t flat_idx) const {
  if (!bin_cardinalities.empty()) return bin_cardinalities[flat_idx];
  return flat_idx == TotalBins() ? bitmap_index.tombstone_bitmap.cardinality()
                                 : bitmap_index.bitmaps[flat_idx].cardinality();
}

uint64_t SABIReader::TombstoneCardinality() const {
  return BinCardinality(TotalBins());
}

const roaring::Roaring* SABIReader::Bin(uint32_t flat_idx, SABIPinnedBin* pin) {
  if (mode_ == SABIReaderMode::kResident) {
    return flat_idx == TotalBins() ? &bitmap_index.tombstone_bitmap
                                   : &bitmap_index.bitmaps[flat_idx];
  }
  assert(table_ != nullptr && pin != nullptr);
  const auto* rep = table_->get_rep();
  const uint64_t file_off =
      rep->udi_handle.offset() + bitmap_offsets_[flat_idx];
  const uint32_t size = bitmap_sizes_[flat_idx];
  // no_block_cache=true (or a caller-supplied null block_cache) leaves this
  // null; BitLSM's ctor takes caller-supplied table options, so it's
  // reachable (see block_prefetch_queue.cpp's InBlockCache for the same
  // guard). Skip the lookup/insert entirely below and hand the decoded bin
  // to the pin instead.
  Cache* cache = rep->table_options.block_cache.get();
  const CacheKey key = rep->base_cache_key.WithOffset(file_off >> 2);

  if (cache != nullptr) {
    if (Cache::Handle* h =
            cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr)) {
      auto* bin = static_cast<SABICachedBin*>(cache->Value(h));
      g_bin_hits.fetch_add(1, memory_order_relaxed);
      pin->cache = cache;
      pin->handle = h;
      pin->view = &bin->view;
      return pin->view;
    }
  }

  auto bin = make_unique<SABICachedBin>();
  void* raw = nullptr;
  if (posix_memalign(&raw, 32, size == 0 ? 32 : size) != 0) return nullptr;
  bin->buf.reset(static_cast<char*>(raw));
  Slice result;
  // One read of the bin's exact extent at its absolute file offset; the
  // reader realigns for direct I/O itself, so a bin never becomes N
  // straddling page reads (the #45 blob-relative-grid mistake).
  IOStatus s = rep->file->Read(IOOptions(), file_off, size, &result,
                               bin->buf.get(), /*aligned_buf=*/nullptr);
  if (!s.ok() || result.size() < size) return nullptr;
  if (result.data() != bin->buf.get()) {
    memcpy(bin->buf.get(), result.data(), size);
  }
  // v4.7.0 frozenView returns non-const Roaring: this move-assigns, so the
  // view aliases buf (no clone).
  static_assert(
      std::is_same_v<decltype(roaring::Roaring::frozenView(nullptr, 0)),
                     roaring::Roaring>,
      "frozenView must return non-const Roaring; a const return would "
      "copy-assign and clone");
  bin->view = Roaring::frozenView(bin->buf.get(), size);
  bin->charge = sizeof(SABICachedBin) + malloc_usable_size(bin->buf.get());
  g_bin_misses.fetch_add(1, memory_order_relaxed);
  g_bytes_read.fetch_add(size, memory_order_relaxed);
  g_bitmaps_loaded.fetch_add(1, memory_order_relaxed);

  if (cache == nullptr) {
    pin->cache = nullptr;
    pin->handle = nullptr;
    pin->owned = std::move(bin);
    pin->view = &pin->owned->view;
    return pin->view;
  }

  Cache::Handle* h = nullptr;
  Status is = cache->Insert(key.AsSlice(), bin.get(), SABICachedBinHelper(),
                            bin->charge, &h);
  pin->cache = cache;
  if (is.ok()) {
    SABICachedBin* entered = bin.release();  // cache owns it now
    pin->handle = h;
    pin->view = &entered->view;
  } else {
    // Refused insert (strict capacity): the pin owns the bin instead.
    pin->handle = nullptr;
    pin->owned = std::move(bin);
    pin->view = &pin->owned->view;
  }
  return pin->view;
}

// The memory usage of the index, including the size of the raw contents and
// any other heap data structures allocated by the reader
size_t SABIReader::ApproximateMemoryUsage() const {
  size_t usage = sizeof(*this);

  // Frozen bitmap storage: actual allocated size, including allocator
  // rounding (buffers come from posix_memalign in the constructor).
  usage += managed_buffers_.capacity() * sizeof(AlignedPtr);
  for (const auto& buf : managed_buffers_)
    usage += malloc_usable_size(buf.get());

  // Schema residue parsed out of the blob's directory.
  usage += schema_.roles.capacity() * sizeof(AttrRole);

  usage += bitmap_index.bitmaps.capacity() * sizeof(roaring::Roaring);
  usage += bitmap_index.bitmap_nums.capacity() * sizeof(uint32_t);
  usage += bitmap_index.binning_policy.capacity() *
           sizeof(decltype(bitmap_index.binning_policy)::value_type);
  for (const auto& policy : bitmap_index.binning_policy) {
    if (const auto* ord = std::get_if<vector<uint64_t>>(&policy)) {
      usage += ord->capacity() * sizeof(uint64_t);
    } else if (const auto* cat =
                   std::get_if<vector<pair<string, uint32_t>>>(&policy)) {
      usage += cat->capacity() * sizeof(pair<string, uint32_t>);
      // Count heap allocations only; short values live inline (SSO), and an
      // empty string's capacity is exactly that inline budget.
      static const size_t kSSOCapacity = string().capacity();
      for (const auto& entry : *cat)
        if (entry.first.capacity() > kSSOCapacity)
          usage += entry.first.capacity() + 1;  // + NUL
    }
  }
  usage += data_entries_cnt_psum.capacity() * sizeof(uint32_t);
  usage += block_handles.capacity() * sizeof(BlockHandle);
  usage += distinct_cnts.capacity() * sizeof(uint64_t);
  usage += bin_cardinalities.capacity() * sizeof(uint32_t);
  usage += bitmap_offsets_.capacity() * sizeof(uint32_t);
  usage += bitmap_sizes_.capacity() * sizeof(uint32_t);

  // Frozen views allocate a separate metadata arena (the bitmap struct plus
  // the container pointer array); the keys, typecodes and the container
  // payloads themselves live in the frozen buffer already counted via
  // managed_buffers_. Approximate the arena from the container count.
  auto frozen_arena_usage = [](const roaring::Roaring& rb) -> size_t {
    size_t n_containers = rb.roaring.high_low_container.size;
    return sizeof(roaring::api::roaring_bitmap_t) +
           n_containers * (sizeof(void*) + 16);
  };
  for (const auto& rb : bitmap_index.bitmaps) usage += frozen_arena_usage(rb);
  usage += frozen_arena_usage(bitmap_index.tombstone_bitmap);

  return usage;
};

void SABIReader::Dump() {
  cout << "==== SABI Dump ====\n";
  cout << "bitmap count: " << bitmap_index.bitmaps.size() << "\n";
  cout << "bitmap_nums: \n";
  for (uint32_t i = 0; i < bitmap_index.bitmap_nums.size(); ++i)
    cout << "\t" << bitmap_index.bitmap_nums[i] << ", ";
  cout << "\n";
}

uint32_t SABIReader::AttrBinOffset(uint32_t attr_idx) const {
  uint32_t offset = 0;
  for (uint32_t i = 0; i < attr_idx; ++i) offset += bitmap_index.bitmap_nums[i];
  return offset;
}

bool SABIReader::OrderedHistogram(uint32_t attr_idx,
                                  OrderedAttrHistogram* out) const {
  if (attr_idx >= schema_.attr_num()) return false;
  if (schema_.roles[attr_idx] != AttrRole::ORDERED) return false;

  uint32_t bin_offset = AttrBinOffset(attr_idx);
  uint32_t bins = bitmap_index.bitmap_nums[attr_idx];

  std::vector<uint64_t> counts(bins);
  uint64_t total = 0;
  for (uint32_t b = 0; b < bins; ++b) {
    counts[b] = BinCardinality(bin_offset + b);
    total += counts[b];
  }
  // Zero binned rows (e.g. every row NULL): the stored boundaries come from
  // an empty t-digest and carry no information.
  if (total == 0) return false;

  out->boundaries =
      std::get<std::vector<uint64_t>>(bitmap_index.binning_policy[attr_idx]);
  out->counts = std::move(counts);
  out->distinct = distinct_cnts[attr_idx];  // 0 on v5 blobs = unknown
  return true;
}

bool SABIReader::UnorderedValueCounts(uint32_t attr_idx,
                                      UnorderedAttrValueCounts* out) const {
  if (attr_idx >= schema_.attr_num()) return false;
  if (schema_.roles[attr_idx] != AttrRole::UNORDERED) return false;

  const auto& entries = std::get<std::vector<std::pair<std::string, uint32_t>>>(
      bitmap_index.binning_policy[attr_idx]);
  if (entries.empty()) return false;  // no interned values (e.g. all NULL)

  uint32_t bin_offset = AttrBinOffset(attr_idx);
  uint32_t bins = bitmap_index.bitmap_nums[attr_idx];

  // Only per-bin cardinality is persisted, so values sharing a bin split its
  // mass uniformly (exact when a value has the bin to itself).
  std::vector<uint32_t> values_in_bin(bins, 0);
  for (const auto& e : entries) values_in_bin[e.second]++;
  std::vector<uint64_t> bin_card(bins);
  uint64_t total = 0;
  for (uint32_t b = 0; b < bins; ++b) {
    bin_card[b] = BinCardinality(bin_offset + b);
    total += bin_card[b];
  }
  if (total == 0) return false;

  out->value_counts.clear();
  out->value_counts.reserve(entries.size());
  for (const auto& e : entries)
    out->value_counts.emplace_back(
        e.first,
        static_cast<double>(bin_card[e.second]) / values_in_bin[e.second]);
  return true;
}

bool SABIReader::QueryCanMatch(const SABIQuery& q) const {
  for (const auto& clause : q.clause_groups) {
    if (clause.empty()) continue;  // empty clause is trivially satisfiable
    // Clause (OR) is impossible iff every condition in it is individually
    // impossible against this SST's binning boundaries.
    bool clause_impossible = true;
    for (const auto& cond : clause) {
      if (!ConditionImpossible(cond, schema_, bitmap_index)) {
        clause_impossible = false;
        break;
      }
    }
    if (clause_impossible) return false;
  }
  return true;
}

}  // namespace bit_lsm
