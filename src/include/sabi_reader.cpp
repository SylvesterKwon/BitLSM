#include "table/format.h"
#include "util/coding.h"
#include "util/coding_lean.h"
#include <cstdint>
#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <iostream>
#include <sabi.h>
#include <sys/types.h>

using namespace std;
using namespace rocksdb;
using namespace roaring;

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

Status SABIUDIIterator::NextAndGetResult(IterateResult* result) {};

UserDefinedIndexBuilder::BlockHandle SABIUDIIterator::value() {};

// ========================================================================
// SABIReader Implementation
// ========================================================================

SABIReader::SABIReader(Slice& index_block, BitLSMOptions options)
    : options_(options) {
  // 1. Read footer
  uint32_t index_entries_cnt_ = DecodeFixed32(
      index_block.data() + index_block.size() - 3 * sizeof(uint32_t));
  uint32_t bitmap_indexoffset_offset = DecodeFixed32(
      index_block.data() + index_block.size() - 2 * sizeof(uint32_t));
  uint32_t binning_policy_offset_offset = DecodeFixed32(
      index_block.data() + index_block.size() - 1 * sizeof(uint32_t));

  data_entries_cnt_psum.resize(index_entries_cnt_);
  block_handles.resize(index_entries_cnt_);

  // 2. Read binning policy
  uint32_t binning_policy_offset_cnt =
      DecodeFixed32(index_block.data() + binning_policy_offset_offset);
  uint32_t binning_policy_cnt = binning_policy_offset_cnt - 1;
  bitmap_index.binning_policy.resize(binning_policy_cnt);
  bitmap_index.bitmap_nums.resize(binning_policy_cnt);
  assert(options_.sk_num == binning_policy_cnt);

  for (uint32_t i = 0; i < binning_policy_cnt; ++i) {
    uint32_t cur_binning_policy_offset =
        DecodeFixed32(index_block.data() + binning_policy_offset_offset +
                      (i + 1) * sizeof(uint32_t));
    uint32_t cur_binning_policy_size =
        DecodeFixed32(index_block.data() + cur_binning_policy_offset);
    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      bitmap_index.bitmap_nums[i] = cur_binning_policy_size;
    } else {
      // since it uses one more array to represent boundary
      bitmap_index.bitmap_nums[i] = cur_binning_policy_size - 1;
    }

    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      // read {length prefixed string + uint32t (bin number)}
      const char* ptr =
          index_block.data() + cur_binning_policy_offset + sizeof(uint32_t);
      vector<pair<string, uint32_t>> cur_binning_policy(
          cur_binning_policy_size);
      for (uint32_t j = 0; j < cur_binning_policy_size; ++j) {
        uint32_t key_len = 0;
        // requires at least 5 bytes
        const char* key_start = GetVarint32Ptr(ptr, ptr + 5, &key_len);
        string key(key_start, key_len);
        ptr = key_start + key_len;
        cur_binning_policy[j] = {key, DecodeFixed32(ptr)};
        ptr += sizeof(uint32_t);
      }
      bitmap_index.binning_policy[i] = std::move(cur_binning_policy);
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      vector<double> cur_binning_policy(cur_binning_policy_size);
      for (uint32_t j = 0; j < cur_binning_policy_size; ++j) {
        uint64_t val_int =
            DecodeFixed64(index_block.data() + cur_binning_policy_offset +
                          sizeof(uint32_t) + j * sizeof(double));
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
  bitmap_index.bitmaps.resize(bitmaps_cnt);
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
    bitmap_index.bitmaps[i] = Roaring::frozenView(
        reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
    managed_buffers_.push_back(
        std::move(managed_aligned_ptr)); // move pointer ownership
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

unique_ptr<UserDefinedIndexIterator>
SABIReader::NewIterator(const ReadOptions& read_options) {
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

} // namespace bit_lsm
