#include <sabi.h>
#include <sys/types.h>

#include <algorithm>
#include <cmath>
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
// described by `bm` and `opts`. Returns false if uncertain or unsupported.
bool ConditionImpossible(const bit_lsm::QueryCondition& cond,
                         const bit_lsm::BitLSMOptions& opts,
                         const bit_lsm::BitmapIndex& bm) {
  uint32_t idx = cond.attr_idx;
  if (idx >= bm.binning_policy.size()) return false;
  if (idx >= opts.attr_types.size()) return false;

  if (opts.attr_types[idx] == bit_lsm::AttrType::ORDERED) {
    if (!std::holds_alternative<std::vector<double>>(bm.binning_policy[idx]))
      return false;
    const auto& bounds = std::get<std::vector<double>>(bm.binning_policy[idx]);
    if (bounds.size() < 2) return false;
    double mn = bounds.front(), mx = bounds.back();
    if (std::isnan(mn) || std::isnan(mx)) return false;

    if (!std::holds_alternative<double>(cond.value)) return false;
    double val = std::get<double>(cond.value);

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
  } else if (opts.attr_types[idx] == bit_lsm::AttrType::UNORDERED) {
    if (cond.op != bit_lsm::CompareOp::EQUAL) return false;
    if (!std::holds_alternative<std::vector<std::pair<std::string, uint32_t>>>(
            bm.binning_policy[idx]))
      return false;
    if (!std::holds_alternative<std::string>(cond.value)) return false;
    const auto& entries =
        std::get<std::vector<std::pair<std::string, uint32_t>>>(
            bm.binning_policy[idx]);
    const std::string& val = std::get<std::string>(cond.value);
    auto it =
        std::lower_bound(entries.begin(), entries.end(), val,
                         [](const std::pair<std::string, uint32_t>& e,
                            const std::string& v) { return e.first < v; });
    return !(it != entries.end() && it->first == val);
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

SABIReader::SABIReader(Slice& index_block, BitLSMOptions options)
    : options_(options) {
  // 1. Read footer: [index footer 3xu32][version u32][magic u32]
  // (version/magic already validated by SABIFactory::NewReader)
  assert(index_block.size() >= 5 * sizeof(uint32_t) &&
         DecodeFixed32(index_block.data() + index_block.size() -
                       sizeof(uint32_t)) == kSABIFooterMagic);
  const char* footer_end =
      index_block.data() + index_block.size() - 2 * sizeof(uint32_t);
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
  assert(options_.attr_num == binning_policy_cnt);

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

    if (options_.attr_types[i] == AttrType::UNORDERED) {
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
    } else if (options_.attr_types[i] == AttrType::ORDERED) {
      vector<double> cur_binning_policy(cur_binning_policy_entry_count);
      for (uint32_t j = 0; j < cur_binning_policy_entry_count; ++j) {
        uint64_t val_int = DecodeFixed64(ptr + j * sizeof(double));
        memcpy(&cur_binning_policy[j], &val_int, sizeof(double));
      }
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

bool SABIReader::QueryCanMatch(const BitLSMQuery& q,
                               const BitLSMOptions& opts) const {
  for (const auto& clause : q.clause_groups) {
    if (clause.empty()) continue;  // empty clause is trivially satisfiable
    // Clause (OR) is impossible iff every condition in it is individually
    // impossible against this SST's binning boundaries.
    bool clause_impossible = true;
    for (const auto& cond : clause) {
      if (!ConditionImpossible(cond, opts, bitmap_index)) {
        clause_impossible = false;
        break;
      }
    }
    if (clause_impossible) return false;
  }
  return true;
}

}  // namespace bit_lsm
