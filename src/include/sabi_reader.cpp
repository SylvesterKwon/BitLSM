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

SABIReader::SABIReader(Slice& index_block, SABISchema schema)
    : schema_(std::move(schema)) {
  // 1. Read footer:
  //    [index footer 3xu32][spec_hash u32][version u32][magic u32]
  // (spec_hash/version/magic already validated by SABIFactory::NewReader)
  assert(index_block.size() >= 6 * sizeof(uint32_t) &&
         DecodeFixed32(index_block.data() + index_block.size() -
                       sizeof(uint32_t)) == kSABIFooterMagic);
  const char* footer_end =
      index_block.data() + index_block.size() - 3 * sizeof(uint32_t);
  uint32_t index_entries_cnt_ =
      DecodeFixed32(footer_end - 3 * sizeof(uint32_t));
  uint32_t bitmap_indexoffset_offset =
      DecodeFixed32(footer_end - 2 * sizeof(uint32_t));
  uint32_t binning_policy_offset_offset =
      DecodeFixed32(footer_end - 1 * sizeof(uint32_t));

  data_entries_cnt_psum.resize(index_entries_cnt_);
  block_handles.resize(index_entries_cnt_);

  // 2. Read binning policy
  uint32_t binning_policy_offset_cnt =
      DecodeFixed32(index_block.data() + binning_policy_offset_offset);
  uint32_t binning_policy_cnt = binning_policy_offset_cnt - 1;
  bitmap_index.binning_policy.resize(binning_policy_cnt);
  bitmap_index.bitmap_nums.resize(binning_policy_cnt);
  assert(schema_.attr_num() == binning_policy_cnt);

  for (uint32_t i = 0; i < binning_policy_cnt; ++i) {
    uint32_t cur_binning_policy_offset =
        DecodeFixed32(index_block.data() + binning_policy_offset_offset +
                      (i + 1) * sizeof(uint32_t));

    // Get bin count
    bitmap_index.bitmap_nums[i] =
        DecodeFixed32(index_block.data() + cur_binning_policy_offset);
    uint32_t cur_binning_policy_entry_count = DecodeFixed32(
        index_block.data() + cur_binning_policy_offset + sizeof(uint32_t));
    const char* ptr =
        index_block.data() + cur_binning_policy_offset + 2 * sizeof(uint32_t);

    if (schema_.roles[i] == AttrRole::UNORDERED) {
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
      vector<uint64_t> cur_binning_policy(cur_binning_policy_entry_count);
      for (uint32_t j = 0; j < cur_binning_policy_entry_count; ++j)
        cur_binning_policy[j] = DecodeFixed64(ptr + j * sizeof(uint64_t));
      bitmap_index.binning_policy[i] = std::move(cur_binning_policy);
    } else {
      assert(false);
    }
  }

  // 3. Read bitmap index offset
  uint32_t bitmap_offset_cnt =
      DecodeFixed32(index_block.data() + bitmap_indexoffset_offset);
  uint32_t bitmaps_cnt = bitmap_offset_cnt - 1;
  bitmap_index.bitmaps.resize(bitmaps_cnt -
                              1);  // the last bitmap is for tombstone
  vector<uint32_t> bitmap_offsets(bitmap_offset_cnt);
  for (uint32_t i = 0; i < bitmap_offset_cnt; ++i) {
    bitmap_offsets[i] =
        DecodeFixed32(index_block.data() + bitmap_indexoffset_offset +
                      (i + 1) * sizeof(uint32_t));
  }
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

  // 4. Read index block related informaiton
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
