#include <sabi.h>
#include <sys/types.h>

#include <algorithm>
#include <cstdint>
#include <iostream>

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

}  // namespace

namespace bit_lsm {

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

SABIReader::SABIReader(Slice& index_block) {
  // 1. Read footer: [directory_off u32][version u32][magic u32]
  // (magic/version/directory bounds already validated by
  // SABIFactory::NewReader)
  assert(index_block.size() >= 3 * sizeof(uint32_t) &&
         DecodeFixed32(index_block.data() + index_block.size() -
                       sizeof(uint32_t)) == kSABIFooterMagic);
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
  vector<uint32_t> policy_offsets(attr_num + 1);
  for (uint32_t i = 0; i <= attr_num; ++i) {
    policy_offsets[i] = DecodeFixed32(dir);
    dir += sizeof(uint32_t);
  }
  uint32_t bitmaps_cnt = total_bins + 1;  // + tombstone bitmap
  vector<uint32_t> bitmap_offsets(bitmaps_cnt + 1);
  for (uint32_t i = 0; i <= bitmaps_cnt; ++i) {
    bitmap_offsets[i] = DecodeFixed32(dir);
    dir += sizeof(uint32_t);
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

  // 4. Read frozen bitmaps
  bitmap_index.bitmaps.resize(bitmaps_cnt -
                              1);  // the last bitmap is for tombstone
  for (uint32_t i = 0; i < bitmaps_cnt; ++i) {
    uint32_t size = bitmap_offsets[i + 1] - bitmap_offsets[i];
    const char* raw_ptr = index_block.data() + bitmap_offsets[i];

    // 32 bytes alignment
    void* aligned_ptr = nullptr;
    posix_memalign(&aligned_ptr, 32, size);
    AlignedPtr managed_aligned_ptr(static_cast<char*>(aligned_ptr), std::free);
    memcpy(managed_aligned_ptr.get(), raw_ptr, size);
    if (i < bitmaps_cnt - 1) {
      bitmap_index.bitmaps[i] = Roaring::frozenView(
          reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
    } else {
      // The last bitmap is the tombstone bitmap
      bitmap_index.tombstone_bitmap = Roaring::frozenView(
          reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
    }
    managed_buffers_.push_back(
        std::move(managed_aligned_ptr));  // move pointer ownership
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

// The memory usage of the index, including the size of the raw contents and
// any other heap data structures allocated by the reader
size_t SABIReader::ApproximateMemoryUsage() const {
  // TODO: implement this
  return 0;
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
    counts[b] = bitmap_index.bitmaps[bin_offset + b].cardinality();
    total += counts[b];
  }
  // Zero binned rows (e.g. every row NULL): the stored boundaries come from
  // an empty t-digest and carry no information.
  if (total == 0) return false;

  out->boundaries =
      std::get<std::vector<uint64_t>>(bitmap_index.binning_policy[attr_idx]);
  out->counts = std::move(counts);
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
    bin_card[b] = bitmap_index.bitmaps[bin_offset + b].cardinality();
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
