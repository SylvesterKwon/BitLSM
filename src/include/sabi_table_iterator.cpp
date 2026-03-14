#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/format.h"
#include <bit_lsm_query.h>
#include <cstdint>
#include <iostream>
#include <memory>
#define TEST_CACHE_LINE_SIZE                                                   \
  64 // To avoid compile error when using roaring.hh &
     // block_based_table_reader.h together

#include "roaring.hh"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_based_table_reader_impl.h" // Required: provides NewDataBlockIterator<> template definition
#include <bit_lsm_iterator.h>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;
using namespace roaring;

void SABITableIterator::GetAllByIndexesFromDataBlock(
    const BlockHandle& bh, vector<uint32_t>& indexes,
    vector<PinnableSlice>& out_keys, vector<PinnableSlice>& out_values) {
  Status s;
  out_keys.clear();
  out_values.clear();
  out_keys.resize(indexes.size());
  out_values.resize(indexes.size());

  // Delete previous block iterator to unpin previous block value
  DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
      ReadOptions(), bh, nullptr, BlockType::kData, nullptr, nullptr, nullptr,
      false, false, s, true);
  biter_.reset(new_biter);

  uint32_t cur_checkpoint =
      UINT32_MAX; // UINT32_MAX means no valid checkpoint is used
  uint32_t cur_offset = 0;
  uint32_t result_idx = 0;

  for (uint32_t i = 0; i < indexes.size(); i++) {
    uint32_t target_checkpoint = indexes[i] / block_restart_interval_;
    if (cur_checkpoint != target_checkpoint) {
      cur_checkpoint = target_checkpoint;
      cur_offset = 0;
      biter_->SeekToRestartPoint(cur_checkpoint);
      biter_->Next(); // need to call Next once to access real data
    }
    uint32_t target_offset = indexes[i] % block_restart_interval_;
    while (cur_offset < target_offset && biter_->Valid()) {
      biter_->Next();
      cur_offset++;
    }
    if (biter_->Valid()) {
      // since key use delta-encoding and bufffer will be overwritten, we can't
      // zero-copy it
      out_keys[i].PinSelf(biter_->key());
      out_values[i].PinSlice(biter_->value(), nullptr);
    } else {
      assert(false);
    }
  }
}

SABITableIterator::SABITableIterator(BlockBasedTable* bbt,
                                     BitLSMOptions options, BitLSMQuery query)
    : options_(options), bbt_(bbt), query_(std::move(query)),
      index_reader_(bbt_->get_rep()->index_reader.get()),
      sabi_reader_(static_cast<SABIReader*>(index_reader_->GetUDIReader())),
      // 1. Build query bitmap
      query_bitmap_(GetBitmapFromQuery(query_)),
      bitmap_iter_(query_bitmap_.begin()), bitmap_end_(query_bitmap_.end()) {
  // Sort query condition by attr_idx
  // This is for using forward scan while value validation

  // Query is sorted in attr_idx order
  block_restart_interval_ =
      bbt->get_rep()->table_options.block_restart_interval;

  // 2. Get target block handles (binary search)
  int64_t last_added_block_idx = -1;
  auto psum_begin = sabi_reader_->data_entries_cnt_psum.begin();
  auto psum_end = sabi_reader_->data_entries_cnt_psum.end();
  for (uint32_t target_idx : query_bitmap_) {
    // cout << "target_idx: " << target_idx << "\n";
    // target_idx is 0-based index, psum array is 1-based count array
    // so upper_bound is always right
    auto it = std::upper_bound(psum_begin, psum_end, target_idx);
    if (it != psum_end) {
      int64_t block_idx =
          std::distance(sabi_reader_->data_entries_cnt_psum.begin(), it);
      if (block_idx != last_added_block_idx) {
        target_blocks_.push_back(
            {block_idx, sabi_reader_->block_handles[block_idx]});
        last_added_block_idx = block_idx;
        psum_begin = it;
      }
    } else {
      assert(false);
    }
  }
  // Debug block hit ratio
  // sabi_reader_->Dump();
  // cout << "\n";
  // cout << "total_bhs_size: " << sabi_reader_->data_entries_cnt_psum.size()
  //      << "\n";
  // cout << "target_blocks_size: " << target_blocks_.size() << "\n";
  // cout << "("
  //      << (double)target_blocks_.size() /
  //             sabi_reader_->data_entries_cnt_psum.size()
  //      << ")\n";
}

roaring::Roaring
SABITableIterator::GetBitmapFromQuery(const BitLSMQuery& query) {
  roaring::Roaring result;
  if (query.conditions.empty())
    return result;

  bool is_first_condition = true;
  vector<const roaring::Roaring*> bitmap_ptrs_buf;
  for (const auto& cond : query.conditions) {

    // 1. Create bitmap for current query condition
    roaring::Roaring cur_cond_bitmap;
    const AttrType& cur_attr_type = options_.attr_types[cond.attr_idx];

    uint32_t bitmap_offset = 0;
    for (uint32_t i = 0; i < cond.attr_idx; ++i)
      bitmap_offset += sabi_reader_->bitmap_index.bitmap_nums[i];

    if (cur_attr_type == AttrType::CATEGORICAL) {
      // We only support EQUAL for categorical type
      // Maybe support OR clause
      if (cond.op != CompareOp::EQUAL)
        assert(false);
      const string& value = std::get<string>(cond.value);
      vector<pair<string, uint32_t>>& cur_attr_binning_policy =
          get<vector<pair<string, uint32_t>>>(
              sabi_reader_->bitmap_index.binning_policy[cond.attr_idx]);
      auto it = std::lower_bound(
          cur_attr_binning_policy.begin(), cur_attr_binning_policy.end(), value,
          [](const pair<string, uint32_t>& policy_entry, const string& val) {
            return policy_entry.first < val;
          });
      if (it != cur_attr_binning_policy.end() && it->first == value) {
        // Found bitmap for given query condition
        uint32_t local_bin_idx = it->second;
        cur_cond_bitmap =
            sabi_reader_->bitmap_index.bitmaps[bitmap_offset + local_bin_idx];
      } else {
        // No bitmap for given query condition, leave it empty bitmap
        // noop
      }
    } else if (cur_attr_type == AttrType::CONTINUOUS) {
      const double& value = std::get<double>(cond.value);
      const vector<double>& boundaries = std::get<vector<double>>(
          sabi_reader_->bitmap_index.binning_policy[cond.attr_idx]);
      uint32_t num_bins = sabi_reader_->bitmap_index.bitmap_nums[cond.attr_idx];

      // Find bin index by value
      // upper_bound: value보다 큰 첫 번째 경계값의 위치
      auto it = std::upper_bound(boundaries.begin(), boundaries.end(), value);

      int32_t target_bin_idx;
      if (it == boundaries.begin()) {
        // Case A: Given value is smaller than leftmost bin (virtual bin -1)
        target_bin_idx = -1;
      } else if (it == boundaries.end()) {
        // Case B: Given value is larger than rightmost bin (virtual bin N)
        target_bin_idx = static_cast<int32_t>(num_bins);
      } else {
        // Case C: Else
        target_bin_idx =
            static_cast<int32_t>(std::distance(boundaries.begin(), it)) - 1;
      }
      // Why using virtual vin -1, N?: To unify range logic without complex
      // branching. e.g., In case of N=10, [10,10]. Range will clamped into
      // inverted range [10,9]. This means empty set.

      // 2. Set raw range based on operator
      int32_t start_bin, end_bin;
      switch (cond.op) {
      case CompareOp::EQUAL:
        start_bin = target_bin_idx;
        end_bin = target_bin_idx;
        break;
      case CompareOp::GREATER_EQUAL: // >= value
      case CompareOp::GREATER:       // > value
        start_bin = target_bin_idx;
        end_bin = static_cast<int32_t>(num_bins) - 1;
        break;
      case CompareOp::LESS_EQUAL: // <= value
      case CompareOp::LESS:       // < value
        start_bin = 0;
        end_bin = target_bin_idx;
        break;

      default:
        assert(false);
      }
      // Clamp range
      if (start_bin < 0)
        start_bin = 0;
      if (static_cast<int32_t>(num_bins) <= end_bin)
        end_bin = static_cast<int32_t>(num_bins) - 1;

      // 3. Merge bitmap (OR)
      uint32_t num_bitmaps = end_bin - start_bin + 1;
      bitmap_ptrs_buf.clear();
      bitmap_ptrs_buf.reserve(num_bitmaps);
      for (int32_t i = start_bin; i <= end_bin; ++i)
        bitmap_ptrs_buf.push_back(
            &(sabi_reader_->bitmap_index.bitmaps[bitmap_offset + i]));
      if (!bitmap_ptrs_buf.empty()) {
        cur_cond_bitmap = roaring::Roaring::fastunion(bitmap_ptrs_buf.size(),
                                                      bitmap_ptrs_buf.data());
      }
    }

    if (is_first_condition) {
      result = cur_cond_bitmap;
      is_first_condition = false;
    } else {
      result &= cur_cond_bitmap;
    }

    // Optimization: If result bitmap is already empty set, return
    // immediately
    if (result.isEmpty())
      return result;
  }

  // 4. Filter tombstone
  if (!sabi_reader_->bitmap_index.tombstone_bitmap.isEmpty())
    result -= sabi_reader_->bitmap_index.tombstone_bitmap;

  return result;
}

void SABITableIterator::LoadNextBlock() {
  while (true) {
    cur_target_block_idx_++;
    // 1. If there's no block left to read, return it
    if (cur_target_block_idx_ >= target_blocks_.size()) {
      valid_ = false;
      return;
    }

    // 2. Global (SST) index -> Local (Block) ID
    auto& [cur_bh_idx, cur_bh] = target_blocks_[cur_target_block_idx_];
    uint32_t global_start_idx =
        (cur_bh_idx == 0) ? 0
                          : sabi_reader_->data_entries_cnt_psum[cur_bh_idx - 1];
    uint32_t global_end_idx = sabi_reader_->data_entries_cnt_psum[cur_bh_idx];

    local_indexes_.clear();
    while (bitmap_iter_ != bitmap_end_) {
      uint32_t global_id = *bitmap_iter_;
      if (global_id >= global_end_idx)
        break;
      if (global_id >= global_start_idx)
        local_indexes_.push_back(global_id - global_start_idx);

      bitmap_iter_++;
    }

    // 3. Handle no row matching row for current block
    if (local_indexes_.empty())
      continue; // Move to next block

    // 4. Load data block
    keys_buffer_.clear(), values_buffer_.clear();
    GetAllByIndexesFromDataBlock(cur_bh, local_indexes_, keys_buffer_,
                                 values_buffer_);

    // 5. Validate & filter value (two-pointer filtering)
    Status s;
    size_t valid_cursor = 0;
    for (size_t i = 0; i < keys_buffer_.size(); ++i) {
      // 5-1. Skip corrupted key
      ParsedInternalKey ikey;
      s = rocksdb::ParseInternalKey(keys_buffer_[i], &ikey, false);
      if (!s.ok())
        continue;

      // 5-2. MVCC filtering
      if (ikey.sequence > options_.read_seqno) {
        continue;
      }

      // 5-3. Filter tombstone
      if (ikey.type == rocksdb::kTypeDeletion ||
          ikey.type == rocksdb::kTypeSingleDeletion) {
        continue;
      }

      // 5-4. Filter query condition
      if (query_.CheckCondition(values_buffer_[i], options_)) {
        if (i != valid_cursor) {
          keys_buffer_[valid_cursor] = std::move(keys_buffer_[i]);
          values_buffer_[valid_cursor] = std::move(values_buffer_[i]);
        }
        valid_cursor++;
      }
    }
    keys_buffer_.resize(valid_cursor);
    values_buffer_.resize(valid_cursor);

    // 6. Finalize loaded buffer
    if (!keys_buffer_.empty()) {
      buffer_idx_ = 0;
      valid_ = true;
      return;
    }
  }
}

void SABITableIterator::SeekToFirst() {
  cur_target_block_idx_ = -1;
  buffer_idx_ = 0;
  keys_buffer_.clear();
  values_buffer_.clear();
  bitmap_iter_ = query_bitmap_.begin();
  LoadNextBlock();
}

void SABITableIterator::Next() {
  assert(valid_);

  // 1. Move buffer cursor to next buffer entry
  buffer_idx_++;

  // 2. If all buffer entries consumed, load next block
  if (buffer_idx_ >= keys_buffer_.size()) {
    valid_ = false;
    LoadNextBlock();
  }
}

Slice SABITableIterator::key() const {
  assert(Valid());
  return keys_buffer_[buffer_idx_];
}

Slice SABITableIterator::value() const {
  assert(Valid());
  return values_buffer_[buffer_idx_];
}