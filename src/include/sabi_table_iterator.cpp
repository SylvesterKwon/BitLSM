#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/format.h"
#include <charconv>
#include <cstdint>
#include <sabi_query.h>
#define TEST_CACHE_LINE_SIZE                                                   \
  64 // To avoid compile error when using roaring.hh &
     // block_based_table_reader.h together

#include "roaring.hh"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include <iostream>
#include <sabi_iterator.h>

using namespace std;
using namespace rocksdb;
using namespace bitmap_index;
using namespace roaring;

void SABITableIterator::GetAllByIndexesFromDataBlock(
    const BlockHandle& bh, vector<uint32_t>& indexes,
    vector<PinnableSlice>& out_keys, vector<PinnableSlice>& out_values) {
  DataBlockIter biter;
  Status s;
  out_keys.resize(indexes.size());
  out_values.resize(indexes.size());
  // TODO: nullptr 로 미사용중인 옵션을 통해 최적화 가능 여부 확인하기
  bbt_->NewDataBlockIterator(ReadOptions(), bh, &biter, BlockType::kData,
                             nullptr, nullptr, nullptr, false, false, s, true);

  uint32_t cur_checkpoint =
      UINT32_MAX; // UINT32_MAX means no valid checkpoint is used
  uint32_t cur_offset = 0;
  uint32_t result_idx = 0;

  for (uint32_t i = 0; i < indexes.size(); i++) {
    uint32_t target_checkpoint = indexes[i] / block_restart_interval_;
    if (cur_checkpoint != target_checkpoint) {
      cur_checkpoint = target_checkpoint;
      cur_offset = 0;
      biter.SeekToRestartPoint(cur_checkpoint);
      biter.Next(); // need to call Next once to access real data
    }
    uint32_t target_offset = indexes[i] % block_restart_interval_;
    while (cur_offset < target_offset && biter.Valid()) {
      biter.Next();
      cur_offset++;
    }
    if (biter.Valid()) {
      // PinSelf vs PinSlice (zero-copy)
      // TODO(TASK-93): Block cache 사용할 수 있도록 최적화 하기.
      // PinSelf 방식은 hard-copy임
      out_keys[i].PinSelf(biter.key());
      out_values[i].PinSelf(biter.value());
    } else {
      assert(false);
    }
  }
}

SABITableIterator::SABITableIterator(SABIOptions options, BlockBasedTable* bbt,
                                     SABIQuery query)
    : options_(options), bbt_(bbt), query_(std::move(query)),
      index_reader_(bbt_->get_rep()->index_reader.get()),
      sabi_reader_(static_cast<SABIReader*>(index_reader_->GetUDIReader())),
      // 1. Build query bitmap
      query_bitmap_(GetBitmapFromQuery(query_)),
      bitmap_iter_(query_bitmap_.begin()), bitmap_end_(query_bitmap_.end()) {
  // Sort query condition by sk_idx
  // This is for using forward scan while value validation
  // TODO: 상위 컴포넌트에서 쿼리 정렬상태 sk_idx 순으로 넘겨주도록 보장하고
  // 아래 로직 삭제하기
  std::sort(query_.conditions.begin(), query_.conditions.end(),
            [](const QueryCondition& a, const QueryCondition& b) {
              return a.sk_idx < b.sk_idx;
            });
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

roaring::Roaring SABITableIterator::GetBitmapFromQuery(const SABIQuery& query) {
  roaring::Roaring result;
  if (query.conditions.empty())
    return result;

  bool is_first_condition = true;
  for (const auto& cond : query.conditions) {

    // 1. Create bitmap for current query condition
    roaring::Roaring cur_cond_bitmap;
    const SKType& cur_sk_type = options_.sk_types[cond.sk_idx];

    uint32_t bitmap_offset = 0;
    for (uint32_t i = 0; i < cond.sk_idx; ++i)
      bitmap_offset += sabi_reader_->bitmap_index.bitmap_nums[i];

    if (cur_sk_type == SKType::CATEGORICAL) {
      if (cond.op != CompareOp::EQUAL)
        assert(false);
      const string& value = std::get<string>(cond.value);
      vector<pair<string, uint32_t>>& cur_sk_binning_policy =
          get<vector<pair<string, uint32_t>>>(
              sabi_reader_->bitmap_index.binning_policy[cond.sk_idx]);
      auto it = std::lower_bound(
          cur_sk_binning_policy.begin(), cur_sk_binning_policy.end(), value,
          [](const pair<string, uint32_t>& policy_entry, const string& val) {
            return policy_entry.first < val;
          });
      if (it != cur_sk_binning_policy.end() && it->first == value) {
        // Found bitmap for given query condition
        uint32_t local_bin_idx = it->second;
        cur_cond_bitmap =
            sabi_reader_->bitmap_index.bitmaps[bitmap_offset + local_bin_idx];
      } else {
        // No bitmap for given query condition, leave it empty bitmap
        // noop
      }
    } else if (cur_sk_type == SKType::CONTINUOUS) {
      const double& value = std::get<double>(cond.value);
      const vector<double>& boundaries = std::get<vector<double>>(
          sabi_reader_->bitmap_index.binning_policy[cond.sk_idx]);
      uint32_t num_bins = sabi_reader_->bitmap_index.bitmap_nums[cond.sk_idx];

      // 1. Find bin index by value
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
        start_bin = target_bin_idx;
        end_bin = static_cast<int32_t>(num_bins) - 1;
        break;
      case CompareOp::LESS_EQUAL: // <= value
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

      // Debug
      // cout << "s: " << start_bin << ", e: " << end_bin << "\n";

      // 3. Merge bitmap (OR)
      for (int32_t i = start_bin; i <= end_bin; ++i) {
        int32_t global_idx = bitmap_offset + i;
        cur_cond_bitmap |= sabi_reader_->bitmap_index.bitmaps[global_idx];
      }
    }

    if (is_first_condition) {
      result = cur_cond_bitmap;
      is_first_condition = false;
    } else {
      result &= cur_cond_bitmap;
    }
    // Debug: Print cardinality each step
    // cout << "cur_cond_bitmap cardinality: " << cur_cond_bitmap.cardinality()
    //      << "\n";
    // cout << "result cardinality: " << result.cardinality() << "\n";

    // Optimization: If result bitmap is already empty set, return
    // immediately
    if (result.isEmpty())
      return result;
  }

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
    size_t valid_cursor = 0;
    for (size_t i = 0; i < keys_buffer_.size(); ++i) {
      if (CheckCondition(values_buffer_[i])) {
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

bool SABITableIterator::CheckCondition(rocksdb::Slice row_value) {
  if (query_.conditions.empty())
    return true;

  uint32_t cur_sk_idx = 0;
  uint32_t cur_cond_idx = 0;

  // For all query condition
  while (cur_cond_idx < query_.conditions.size()) {
    uint32_t target_sk_idx = query_.conditions[cur_cond_idx].sk_idx;

    // 1. Skip unnecessary sk
    while (cur_sk_idx < target_sk_idx) {
      // If there's no sk left (maybe new kind of sk is queried) return false
      if (row_value.empty())
        return false;
      Slice ignored;
      GetLengthPrefixedSlice(&row_value, &ignored); // Move pointer
      cur_sk_idx++;
    }

    // 2. Read target sk value
    Slice target_sk_val_slice;
    if (!GetLengthPrefixedSlice(&row_value, &target_sk_val_slice))
      return false;
    cur_sk_idx++;
    double val_double;
    bool parsed = false; // To prevent redundant double parse

    // 3. Check all condition for current sk
    while (cur_cond_idx < query_.conditions.size() &&
           query_.conditions[cur_cond_idx].sk_idx == target_sk_idx) {
      const auto& cond = query_.conditions[cur_cond_idx];
      bool match = false;
      const SKType sk_type = options_.sk_types[cond.sk_idx];

      if (sk_type == SKType::CATEGORICAL) {
        const string& query_val = get<string>(cond.value);
        int cmp = target_sk_val_slice.compare(query_val);
        if (cond.op == CompareOp::EQUAL)
          match = (cmp == 0);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (cmp >= 0);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (cmp <= 0);
        else
          assert(false);
      } else if (sk_type == SKType::CONTINUOUS) {
        if (!parsed) {
          auto res = std::from_chars(target_sk_val_slice.data(),
                                     target_sk_val_slice.data() +
                                         target_sk_val_slice.size(),
                                     val_double);
          if (res.ec != std::errc())
            return false;
          parsed = true;
        }
        double query_val = std::get<double>(cond.value);
        if (cond.op == CompareOp::EQUAL)
          match = (val_double == query_val);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (val_double >= query_val);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (val_double <= query_val);
      }
      if (!match)
        return false;
      cur_cond_idx++;
    }
  }
  return true;
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
    cout << "need new block ==========\n";
    LoadNextBlock();
  }
}

bool SABITableIterator::Valid() { return valid_; }

void SABITableIterator::TEST() {
  for (SeekToFirst(); Valid(); Next()) {
    cout << keys_buffer_[buffer_idx_].ToStringView() << "\n";
    TEST_DumpValue(values_buffer_[buffer_idx_]);
  }

  // TEST: 한 data block에 몇개의 entry가 들어가는가 테스트용
  // DataBlockIter biter;
  // Status s;
  // bbt_->NewDataBlockIterator(ReadOptions(), bh, &biter, BlockType::kData,
  //                            nullptr, nullptr, nullptr, false, false, s,
  //                            true);
  // uint32_t cnt = 0;
  // for (biter.SeekToFirst(); biter.Valid(); biter.Next()) {
  //   cnt += 1;
  // }
  // cout << "block cnt: " << cnt << "\n";
}

void SABITableIterator::TEST_DumpValue(Slice input) {
  std::cout << "==== Value Decode Debug (" << input.size() << " bytes) ====\n";
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    rocksdb::Slice part;
    GetLengthPrefixedSlice(&input, &part);
    std::cout << "  [SK:" << i << "] ";

    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      std::cout << "(CAT) : " << part.ToString();
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      // Builder에서 문자열 형태로 저장했으므로 문자열로 출력하되, double 해석
      // 가능 여부 확인
      std::string s = part.ToString();
      std::cout << "(CONT): " << s;
    }
    std::cout << "\n";
  }
}
