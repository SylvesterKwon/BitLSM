#include <malloc.h>  // malloc_usable_size
#include <sabi.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>

#include "block_prefetch_queue.h"  // shared async-unavailable flag
#include "cache/cache_key.h"
#include "file/file_prefetch_buffer.h"
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
    // Impossible iff the interval is empty or disjoint from [mn, mx].
    return cond.win.Empty() || cond.win.hi < mn || cond.win.lo > mx;
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
std::atomic<uint64_t> g_bin_reads{0};
std::atomic<uint64_t> g_bytes_read{0};
std::atomic<uint64_t> g_bitmaps_loaded{0};
std::atomic<uint64_t> g_bin_inserts_refused{0};
std::atomic<uint64_t> g_spans_planned{0};
std::atomic<uint64_t> g_spans_prefetched{0};
std::atomic<uint64_t> g_spans_dropped{0};
}  // namespace

SABIBinCacheStats GetSABIBinCacheStats() {
  return {g_bin_hits.load(memory_order_relaxed),
          g_bin_misses.load(memory_order_relaxed),
          g_bin_reads.load(memory_order_relaxed),
          g_bytes_read.load(memory_order_relaxed),
          g_bitmaps_loaded.load(memory_order_relaxed),
          g_bin_inserts_refused.load(memory_order_relaxed),
          g_spans_planned.load(memory_order_relaxed),
          g_spans_prefetched.load(memory_order_relaxed),
          g_spans_dropped.load(memory_order_relaxed)};
}

void ResetSABIBinCacheStats() {
  g_bin_hits.store(0, memory_order_relaxed);
  g_bin_misses.store(0, memory_order_relaxed);
  g_bin_reads.store(0, memory_order_relaxed);
  g_bytes_read.store(0, memory_order_relaxed);
  g_bitmaps_loaded.store(0, memory_order_relaxed);
  g_bin_inserts_refused.store(0, memory_order_relaxed);
  g_spans_planned.store(0, memory_order_relaxed);
  g_spans_prefetched.store(0, memory_order_relaxed);
  g_spans_dropped.store(0, memory_order_relaxed);
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
  assert(flat_idx <= TotalBins());
  if (!bin_cardinalities.empty()) return bin_cardinalities[flat_idx];
  return flat_idx == TotalBins() ? bitmap_index.tombstone_bitmap.cardinality()
                                 : bitmap_index.bitmaps[flat_idx].cardinality();
}

uint64_t SABIReader::TombstoneCardinality() const {
  return BinCardinality(TotalBins());
}

Cache* SABIReader::BinCache() const {
  // no_block_cache=true (or a caller-supplied null block_cache) leaves this
  // null; BitLSM's ctor takes caller-supplied table options, so it's
  // reachable (see block_prefetch_queue.cpp's InBlockCache for the same
  // guard). ProbeBin/LoadRun then skip lookup/insert entirely and hand
  // decoded bins to their pins instead.
  return table_->get_rep()->table_options.block_cache.get();
}

CacheKey SABIReader::BinCacheKey(uint32_t flat_idx) const {
  const auto* rep = table_->get_rep();
  const uint64_t file_off =
      rep->udi_handle.offset() + bitmap_offsets_[flat_idx];
  return rep->base_cache_key.WithOffset(file_off >> 2);
}

bool SABIReader::BinCached(uint32_t flat_idx) const {
  Cache* cache = BinCache();
  if (cache == nullptr) return false;
  Cache::Handle* h =
      cache->BasicLookup(BinCacheKey(flat_idx).AsSlice(), /*stats=*/nullptr);
  if (h == nullptr) return false;
  cache->Release(h);
  return true;
}

bool SABIReader::ProbeBin(Cache* cache, uint32_t flat_idx,
                          SABIPinnedBin* slot) {
  if (cache == nullptr) return false;
  const CacheKey key = BinCacheKey(flat_idx);
  Cache::Handle* h = cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr);
  if (h == nullptr) return false;
  auto* bin = static_cast<SABICachedBin*>(cache->Value(h));
  g_bin_hits.fetch_add(1, memory_order_relaxed);
  slot->cache = cache;
  slot->handle = h;
  slot->view = &bin->view;
  return true;
}

bool SABIReader::LoadRun(uint32_t a, uint32_t b, Cache* cache,
                         SABIPinnedBin* slots, SABISpanPrefetch* prefetch) {
  const auto* rep = table_->get_rep();
  const uint64_t udi_off = rep->udi_handle.offset();
  // One read covering every missed bin, [padded start of a, exact end of b).
  // Alignment gaps between bins ride along: dead bytes cost far less than
  // the per-bin reads they replace.
  const uint64_t file_off = udi_off + bitmap_offsets_[a];
  const size_t run_len =
      (bitmap_offsets_[b] - bitmap_offsets_[a]) + bitmap_sizes_[b];

  void* raw = nullptr;
  // run_len >= bitmap_sizes_[b] >= 4 (the frozen header), never zero.
  if (posix_memalign(&raw, 32, run_len) != 0) return false;
  // One shared arena for the run: every bin's entry keeps a reference, so
  // the buffer lives until the last sharing entry (cached or pin-owned)
  // dies, whatever order the cache evicts them in.
  std::shared_ptr<char> arena(static_cast<char*>(raw), std::free);

  // A span prefetch may already hold the run's bytes: copy out of its
  // buffer and skip the file read (`reads` counts issued file reads, so a
  // served run adds none). A miss, dead slot or short buffer falls through
  // to the pread below -- identical bytes either way.
  if (prefetch == nullptr ||
      !prefetch->TryConsume(file_off, run_len, arena.get())) {
    Slice result;
    // One read at the run's absolute file offset; the reader realigns for
    // direct I/O itself, so the run never becomes N straddling page reads
    // (the v1 design's blob-relative-grid mistake).
    IOStatus s = rep->file->Read(IOOptions(), file_off, run_len, &result,
                                 arena.get(), /*aligned_buf=*/nullptr);
    // Counts reads issued, not reads succeeded: a failed run still shows up.
    g_bin_reads.fetch_add(1, memory_order_relaxed);
    // A short read fails the whole run: no partial arena is ever cached.
    if (!s.ok() || result.size() < run_len) return false;
    if (result.data() != arena.get()) {
      memcpy(arena.get(), result.data(), run_len);
    }
  }
  g_bytes_read.fetch_add(run_len, memory_order_relaxed);

  // v4.7.0 frozenView returns non-const Roaring: this move-assigns, so the
  // view aliases the arena (no clone).
  static_assert(
      std::is_same_v<decltype(roaring::Roaring::frozenView(nullptr, 0)),
                     roaring::Roaring>,
      "frozenView must return non-const Roaring; a const return would "
      "copy-assign and clone");
  for (uint32_t i = a; i <= b; ++i) {
    auto bin = make_unique<SABICachedBin>();
    bin->arena = arena;
    // v7 32B-aligns every padded start, so the offset difference keeps each
    // in-arena view 32B-aligned as frozenView requires.
    const uint32_t rel = bitmap_offsets_[i] - bitmap_offsets_[a];
    bin->view = Roaring::frozenView(arena.get() + rel, bitmap_sizes_[i]);
    // The padded extent, not the arena size: a span's entries then sum to
    // about one arena, charged exactly once across them.
    bin->charge =
        sizeof(SABICachedBin) + (bitmap_offsets_[i + 1] - bitmap_offsets_[i]);
    g_bin_misses.fetch_add(1, memory_order_relaxed);
    g_bitmaps_loaded.fetch_add(1, memory_order_relaxed);

    SABIPinnedBin* slot = &slots[i - a];
    if (cache == nullptr) {
      slot->owned = std::move(bin);
      slot->view = &slot->owned->view;
      continue;
    }
    const CacheKey key =
        rep->base_cache_key.WithOffset((udi_off + bitmap_offsets_[i]) >> 2);
    Cache::Handle* h = nullptr;
    // HIGH: same convention as RocksDB's own
    // cache_index_and_filter_blocks_with_high_priority, so data-block churn
    // stops evicting the bins that gate it.
    Status is = cache->Insert(key.AsSlice(), bin.get(), SABICachedBinHelper(),
                              bin->charge, &h, Cache::Priority::HIGH);
    slot->cache = cache;
    if (is.ok()) {
      SABICachedBin* entered = bin.release();  // cache owns it now
      slot->handle = h;
      slot->view = &entered->view;
    } else {
      // Refused insert (strict capacity): the pin owns the bin instead; its
      // arena stays shared with whatever run-mates did get in.
      g_bin_inserts_refused.fetch_add(1, memory_order_relaxed);
      slot->handle = nullptr;
      slot->owned = std::move(bin);
      slot->view = &slot->owned->view;
    }
  }
  return true;
}

const roaring::Roaring* SABIReader::Bin(uint32_t flat_idx, SABIPinnedBin* pin,
                                        SABISpanPrefetch* prefetch) {
  if (mode_ == SABIReaderMode::kResident) {
    return flat_idx == TotalBins() ? &bitmap_index.tombstone_bitmap
                                   : &bitmap_index.bitmaps[flat_idx];
  }
  assert(table_ != nullptr && pin != nullptr);
  Cache* cache = BinCache();
  // A span of one: single-bin misses take the same probe/read/insert path
  // as coalesced runs, so there is exactly one miss path to keep correct.
  SABIPinnedBin slot;
  if (!ProbeBin(cache, flat_idx, &slot) &&
      !LoadRun(flat_idx, flat_idx, cache, &slot, prefetch)) {
    return nullptr;
  }
  *pin = std::move(slot);
  return pin->view;
}

bool SABIReader::BinRange(uint32_t first_flat, uint32_t last_flat,
                          std::vector<const roaring::Roaring*>* out_views,
                          std::vector<SABIPinnedBin>* out_pins,
                          SABISpanPrefetch* prefetch) {
  assert(first_flat <= last_flat && last_flat <= TotalBins());
  assert(out_views != nullptr && out_pins != nullptr);
  if (mode_ == SABIReaderMode::kResident) {
    for (uint32_t i = first_flat; i <= last_flat; ++i)
      out_views->push_back(i == TotalBins() ? &bitmap_index.tombstone_bitmap
                                            : &bitmap_index.bitmaps[i]);
    return true;
  }
  assert(table_ != nullptr);
  Cache* cache = BinCache();
  const uint32_t n = last_flat - first_flat + 1;
  vector<SABIPinnedBin> slots(n);
  for (uint32_t i = 0; i < n; ++i) ProbeBin(cache, first_flat + i, &slots[i]);
  // Coalesce the misses into maximal contiguous runs of one file read each.
  for (uint32_t i = 0; i < n; ++i) {
    if (slots[i].view != nullptr) continue;
    uint32_t j = i;
    while (j + 1 < n && slots[j + 1].view == nullptr) ++j;
    if (!LoadRun(first_flat + i, first_flat + j, cache, &slots[i], prefetch)) {
      return false;  // hit pins in `slots` release on unwind
    }
    i = j;
  }
  // Append in flat-idx order (hits and runs interleaved deterministically).
  for (SABIPinnedBin& slot : slots) {
    out_views->push_back(slot.view);
    out_pins->push_back(std::move(slot));
  }
  return true;
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

bool SABIReader::SelectBins(const SABICondition& cond,
                            BinSelection* out) const {
  const AttrRole role = schema_.roles[cond.attr_idx];
  const uint32_t bitmap_offset = AttrBinOffset(cond.attr_idx);

  if (role == AttrRole::UNORDERED) {
    if (cond.op != CompareOp::EQUAL) assert(false);
    const auto& policy = get<vector<pair<string, uint32_t>>>(
        bitmap_index.binning_policy[cond.attr_idx]);
    auto it = std::lower_bound(
        policy.begin(), policy.end(), cond.bytes,
        [](const pair<string, uint32_t>& policy_entry, const string& val) {
          return policy_entry.first < val;
        });
    if (it == policy.end() || it->first != cond.bytes) return false;
    out->first = out->last = bitmap_offset + it->second;
    return true;
  }
  if (role != AttrRole::ORDERED) return false;
  if (cond.win.Empty()) return false;

  const auto& boundaries =
      get<vector<uint64_t>>(bitmap_index.binning_policy[cond.attr_idx]);
  const uint32_t num_bins = bitmap_index.bitmap_nums[cond.attr_idx];

  // Bin holding `value`: index of the last boundary <= value. -1 when value
  // sits below the leftmost boundary; the top bin is closed on the right,
  // matching how the builder bins the maximum value. Strict bounds arrived
  // canonicalized one okey step inward (OkeyInterval::FromOp), so a bound
  // sitting exactly on a threshold lands in the correct neighbor bin here
  // without any threshold-equality special case.
  auto bin_of = [&](uint64_t value) -> int32_t {
    auto it = std::upper_bound(boundaries.begin(), boundaries.end(), value);
    if (it == boundaries.begin()) return -1;
    if (it == boundaries.end()) return static_cast<int32_t>(num_bins) - 1;
    return static_cast<int32_t>(std::distance(boundaries.begin(), it)) - 1;
  };

  const int32_t end_bin = bin_of(cond.win.hi);
  if (end_bin < 0) return false;  // whole interval below the leftmost bin
  int32_t start_bin = bin_of(cond.win.lo);
  if (start_bin < 0) start_bin = 0;
  // bin_of is monotone, so start_bin <= end_bin here; the extent is exactly
  // the bins the interval can touch, boundary bins included (their rows are
  // over-approximated and trimmed by per-row verification).
  out->first = bitmap_offset + static_cast<uint32_t>(start_bin);
  out->last = bitmap_offset + static_cast<uint32_t>(end_bin);
  return true;
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

// ========================================================================
// SABISpanPrefetch Implementation
// ========================================================================

SABISpanPrefetch::SABISpanPrefetch(SABIReader* reader,
                                   const BlockBasedTable* table)
    : reader_(reader), table_(table) {}

SABISpanPrefetch::~SABISpanPrefetch() = default;

void SABISpanPrefetch::PlanAndSubmit(
    const std::vector<BinSelection>& selections) {
  if (AsyncReadUnavailable()) return;

  // Union the selections into byte-contiguous intervals: overlapping and
  // flat-adjacent extents share one read (adjacent bins are adjacent bytes
  // in the blob, whatever attr each belongs to); a gap of even one unwanted
  // bin keeps intervals separate, so no dead bytes ride along between runs.
  std::vector<BinSelection> sorted = selections;
  std::sort(sorted.begin(), sorted.end(),
            [](const BinSelection& x, const BinSelection& y) {
              return x.first < y.first;
            });
  std::vector<BinSelection> intervals;
  for (const BinSelection& sel : sorted) {
    if (!intervals.empty() && sel.first <= intervals.back().last + 1) {
      intervals.back().last = std::max(intervals.back().last, sel.last);
    } else {
      intervals.push_back(sel);
    }
  }

  // Coalesce each interval's cache-missing bins into maximal contiguous
  // runs -- the same grouping BinRange rediscovers when it loads, as long
  // as the cache does not change in between (and when it does, consumers
  // just fall back to their own pread). One cache peek per bin: a run ends
  // on the same lookup that shows the next bin cached.
  std::vector<BinSelection> runs;
  constexpr uint32_t kNoRun = std::numeric_limits<uint32_t>::max();
  for (const BinSelection& iv : intervals) {
    uint32_t run_start = kNoRun;
    for (uint32_t i = iv.first; i <= iv.last; ++i) {
      if (reader_->BinCached(i)) {
        if (run_start != kNoRun) {
          runs.push_back({run_start, i - 1});
          run_start = kNoRun;
        }
      } else if (run_start == kNoRun) {
        run_start = i;
      }
    }
    if (run_start != kNoRun) runs.push_back({run_start, iv.last});
  }

  // A lone run overlaps nothing: the sync pread LoadRun issues anyway is
  // the same single read, so skip the machinery entirely.
  if (runs.size() < 2) return;
  g_spans_planned.fetch_add(runs.size(), memory_order_relaxed);

  // Submit in plan order up to the slot cap. Every planned run that ends
  // up with no slot -- beyond the cap, async known unavailable (the first
  // NotSupported stops the rest, like BlockPrefetchQueue::Submit), or a
  // failed submit -- is dropped: its loads stay synchronous.
  slots_.reserve(std::min(runs.size(), kMaxSlots));
  uint64_t dropped = 0;
  for (size_t r = 0; r < runs.size(); ++r) {
    if (r >= kMaxSlots || AsyncReadUnavailable() ||
        !Submit(runs[r].first, runs[r].last)) {
      ++dropped;
    }
  }
  if (dropped > 0) g_spans_dropped.fetch_add(dropped, memory_order_relaxed);
}

bool SABISpanPrefetch::Submit(uint32_t a, uint32_t b) {
  const BlockBasedTable::Rep* rep = table_->get_rep();
  // The exact byte extent LoadRun computes for [a, b]: padded start of a to
  // the exact end of b.
  const uint64_t offset =
      rep->udi_handle.offset() + reader_->bitmap_offsets_[a];
  const size_t len =
      (reader_->bitmap_offsets_[b] - reader_->bitmap_offsets_[a]) +
      reader_->bitmap_sizes_[b];

  Slot slot;
  // No readahead: this run's bytes and nothing else (the slot setup in
  // block_prefetch_queue.cpp).
  ReadaheadParams params;
  params.num_buffers = 1;
  rep->CreateFilePrefetchBuffer(params, &slot.buf,
                                /*readaheadsize_cb=*/nullptr,
                                FilePrefetchBufferUsage::kUserScanPrefetch);

  IOOptions opts;
  IODebugContext dbg;
  const ReadOptions read_options;
  if (!rep->file->PrepareIOOptions(read_options, opts, &dbg).ok()) return false;

  Slice result;
  // TryAgain = submitted and in flight; OK = the buffer already held it.
  const Status s =
      slot.buf->PrefetchAsync(opts, rep->file.get(), offset, len, &result);
  slot.offset = offset;
  slot.len = len;
  slot.remaining = len;
  if (s.IsTryAgain()) {
    slot.state = SlotState::kSubmitted;
  } else if (s.ok() && result.size() >= len) {
    slot.data = result;
    slot.state = SlotState::kPopulated;
  } else {
    if (s.IsNotSupported()) NoteAsyncReadUnavailable();
    return false;  // dropped: LoadRun preads as if the run was never planned
  }
  slots_.push_back(std::move(slot));
  return true;
}

bool SABISpanPrefetch::TryConsume(uint64_t offset, size_t len, char* dst) {
  for (Slot& slot : slots_) {
    if (offset < slot.offset || offset + len > slot.offset + slot.len) {
      continue;  // runs are disjoint: at most one slot can hold the range
    }
    if (slot.state == SlotState::kDead) return false;
    if (slot.state == SlotState::kSubmitted) {
      // First touch: poll the whole run at its submitted offset, the only
      // offset an explicit async submit answers to (any other aborts the
      // buffer -- TryReadFromCacheUntracked). Later sub-ranges serve from
      // the populated buffer with no further I/O.
      const BlockBasedTable::Rep* rep = table_->get_rep();
      IOOptions opts;
      IODebugContext dbg;
      const ReadOptions read_options;
      Slice result;
      Status status;
      const bool ok =
          rep->file->PrepareIOOptions(read_options, opts, &dbg).ok() &&
          slot.buf->TryReadFromCache(opts, rep->file.get(), slot.offset,
                                     slot.len, &result, &status) &&
          status.ok() && result.size() >= slot.len;
      if (!ok) {
        // Failed or short: kill the whole run (and free its buffer now),
        // never serve part of it. The consumer preads instead -- results
        // identical.
        slot.state = SlotState::kDead;
        slot.buf.reset();
        return false;
      }
      slot.data = result;
      slot.state = SlotState::kPopulated;
    }
    memcpy(dst, slot.data.data() + (offset - slot.offset), len);
    g_spans_prefetched.fetch_add(1, memory_order_relaxed);
    // Clamped: with no bin cache the consumed sub-ranges can overlap
    // (nothing records the first load), and over-subtracting would only
    // free early -- later overlaps then pread.
    slot.remaining -= std::min(len, slot.remaining);
    if (slot.remaining == 0) {
      // The whole run has been copied out: free the buffer now instead of
      // holding every run's bytes until the scan ends.
      slot.data = Slice();
      slot.buf.reset();
      slot.state = SlotState::kDead;
    }
    return true;
  }
  return false;
}

}  // namespace bit_lsm
