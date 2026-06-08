#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <sabi.h>
#include <sys/types.h>

#include <iostream>
#include <queue>

#include "bit_lsm_utils.h"
#include "util/coding.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace bit_lsm {
// ========================================================================
// SABIBuilder Implementation
// ========================================================================
SABIBuilder::SABIBuilder(BitLSMOptions options) : options_(options) {
  bitmap_index_.bitmap_nums.resize(options.attr_num, 0);
  bitmap_index_.binning_policy.resize(options.attr_num);
  attr_buf_.reserve(options.attr_num);
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (options_.attr_types[i] == AttrType::CONTINUOUS) {
      attr_buf_.push_back(vector<double>());
    } else {
      attr_buf_.push_back(vector<string>());
    }
  }
};

Slice SABIBuilder::AddIndexEntry(const Slice& last_key_in_current_block,
                                 const Slice* first_key_in_next_block,
                                 const BlockHandle& block_handle,
                                 string* separator_scratch) {
  // Add table KVPairs prefix count / bh.offset / bh.size
  PutFixed32(&index_blob_, data_entries_cnt_);
  PutFixed32(&index_blob_, block_handle.offset);
  PutFixed32(&index_blob_, block_handle.size);
  ++index_entries_cnt_;
  return last_key_in_current_block;
}

void SABIBuilder::OnKeyAdded(const Slice& key, ValueType type,
                             const Slice& value) {
  // 1. Handle tombstone
  bool is_value = true;
  if (type == ValueType::kTypeDeletion ||
      type == ValueType::kTypeSingleDeletion) {
    is_value = false;
  }

  // 2. Buffer original attr data
  if (is_value) {
    std::string_view buffer(value.data(), value.size());
    Slice v = value;
    for (uint32_t i = 0; i < options_.attr_num; ++i) {
      AttrView attr_val = DecodeAttr(options_.attr_types[i], buffer, i);
      if (options_.attr_types[i] == AttrType::CONTINUOUS) {
        double val = get<double>(attr_val);
        get<vector<double>>(attr_buf_[i]).push_back(val);
      } else {
        string_view str_val = get<string_view>(attr_val);
        get<vector<string>>(attr_buf_[i]).emplace_back(str_val);
      }
    }
  } else {
    // If it's a tombstone, we still need to add a dummy value to the buffer for
    // the sake of simplicity in binning policy calculation.
    for (uint32_t i = 0; i < options_.attr_num; ++i) {
      if (options_.attr_types[i] == AttrType::CONTINUOUS) {
        get<vector<double>>(attr_buf_[i]).push_back(0.0);
      } else {
        get<vector<string>>(attr_buf_[i]).emplace_back("");
      }
    }
    // Also add the entry to the tombstone bitmap
    bitmap_index_.tombstone_bitmap.add(data_entries_cnt_);
  }

  // 3. Calculate statistics
  ++data_entries_cnt_;
  total_data_entries_size_uncomp_ += value.size();
  total_data_entries_size_uncomp_ += key.size();
}

void SABIBuilder::SetBinningPolicy() {
  // 1. Set target total bitmap index number
  // total_bitmaps_cnt = attr_num / fpr
  // rho: expected bin selectivity per point query (0, 1]
  // e.g. rho=0.1 means 10 bins per attr on avg.
  uint32_t target_total_bitmaps_cnt =
      (uint32_t)(options_.attr_num / options_.rho);

  // 2. Set # of bitmaps for each attr
  vector<unordered_map<string_view, uint32_t>> cat_buf_map(
      options_.attr_num);  // map for counting each categorical attr's value
  vector<uint32_t> cardinality(options_.attr_num, 0);
  uint32_t cardinality_ub =
      target_total_bitmaps_cnt;  // theoretically max bins for one attr
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (options_.attr_types[i] == AttrType::CATEGORICAL) {
      const auto& str_vec = get<vector<string>>(attr_buf_[i]);
      for (const string& val : str_vec) {
        cat_buf_map[i][val]++;
      }
      cardinality[i] = cat_buf_map[i].size();
    } else {
      const auto& double_vec = get<vector<double>>(attr_buf_[i]);
      unordered_set<double> unique_vals;
      for (double val : double_vec) {
        unique_vals.insert(val);
        if (unique_vals.size() >= cardinality_ub) {
          break;  // Optimization: Upper bound 도달 시 탐색 즉시 종료
        }
      }
      cardinality[i] = unique_vals.size();
    }
  }

  // Weight (importance) vector of attrs.
  // TODO: aggregate read queries to compute weights (currently assumes uniform
  // weights).
  vector<double> query_weight_vector(attr_buf_.size(), 1.0);
  int32_t remaining_budget = target_total_bitmaps_cnt;
  priority_queue<pair<double, uint32_t>> pq;  // {diminishing returns, attr idx}
  uint32_t total_bitmaps_num = 0;

  for (uint32_t i = 0; i < bitmap_index_.bitmap_nums.size(); ++i) {
    // Allocate at least 1 bin (prevent divide by zero)
    bitmap_index_.bitmap_nums[i] = 1;
    remaining_budget--;
    total_bitmaps_num++;
    // Delta cost = - N / b_i * (b_i + 1)
    pq.push({query_weight_vector[i] / (bitmap_index_.bitmap_nums[i] *
                                       (bitmap_index_.bitmap_nums[i] + 1)),
             i});
  }

  while (remaining_budget > 0 && pq.size() > 0) {
    auto [_, idx] = pq.top();
    pq.pop();
    if (cardinality[idx] <= bitmap_index_.bitmap_nums[idx]) continue;

    bitmap_index_.bitmap_nums[idx]++;
    remaining_budget--;
    total_bitmaps_num++;
    pq.push({query_weight_vector[idx] / (bitmap_index_.bitmap_nums[idx] *
                                         (bitmap_index_.bitmap_nums[idx] + 1)),
             idx});
  }
  bitmap_index_.bitmaps.resize(total_bitmaps_num);

  // 3. Set binning boundaries for each attr
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (options_.attr_types[i] == AttrType::CATEGORICAL) {
      // 3-A. Categorical property Binning
      SetCategoricalPropertyBinningPolicy(i, cat_buf_map);
    } else if (options_.attr_types[i] == AttrType::CONTINUOUS) {
      // 3-B. Continuous property Binning
      SetContinuousPropertyBinningPolicy(i);
    } else {
      assert(false);
    }
  }
}

void SABIBuilder::SetCategoricalPropertyBinningPolicy(
    uint32_t i, vector<unordered_map<string_view, uint32_t>>& buf_map) {
  priority_queue<pair<uint32_t, uint32_t>, vector<pair<uint32_t, uint32_t>>,
                 greater<pair<uint32_t, uint32_t>>>
      min_bin_pq;
  vector<pair<string_view, uint32_t>> sorted_items;  // sorted by cnt
  for (const auto& [val, cnt] : buf_map[i]) {
    sorted_items.push_back({val, cnt});
  }
  std::sort(sorted_items.begin(), sorted_items.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  vector<pair<string, uint32_t>> binning;
  binning.reserve(sorted_items.size());
  for (uint32_t j = 0; j < bitmap_index_.bitmap_nums[i]; ++j)
    min_bin_pq.push({0, j});
  for (auto& [val, cnt] : sorted_items) {
    auto [cur_bin_cnt, bin_idx] = min_bin_pq.top();
    min_bin_pq.pop();
    binning.push_back({string(val), bin_idx});
    min_bin_pq.push({cur_bin_cnt + cnt, bin_idx});
  }
  sort(binning.begin(), binning.end());
  bitmap_index_.binning_policy[i] = std::move(binning);
}

void SABIBuilder::SetContinuousPropertyBinningPolicy(uint32_t i) {
  folly::TDigest digest(bitmap_index_.bitmap_nums[i] * 5);

  const auto& v = get<vector<double>>(attr_buf_[i]);
  digest = digest.merge(folly::Range<const double*>(v.data(), v.size()));

  // N+1 boundary points
  vector<double> boundaries(bitmap_index_.bitmap_nums[i] + 1);
  for (uint32_t j = 0; j <= bitmap_index_.bitmap_nums[i]; ++j) {
    double quantile = (double)j / (double)bitmap_index_.bitmap_nums[i];
    boundaries[j] = digest.estimateQuantile(quantile);
  }
  bitmap_index_.binning_policy[i] = std::move(boundaries);
}

void SABIBuilder::CalculateBitmapIndex() {
  uint32_t bin_idx_offset = 0;
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (options_.attr_types[i] == AttrType::CATEGORICAL) {
      const vector<string>& cur_attr_buf = get<vector<string>>(attr_buf_[i]);
      vector<pair<string, uint32_t>>& binning =
          get<vector<pair<string, uint32_t>>>(bitmap_index_.binning_policy[i]);
      for (uint32_t j = 0; j < cur_attr_buf.size(); ++j) {
        const string& key = cur_attr_buf[j];

        auto it =
            lower_bound(binning.begin(), binning.end(), key,
                        [](const pair<string, uint32_t>& element,
                           const string& val) { return element.first < val; });
        if (it != binning.end() && it->first == key) {
          uint32_t bin_idx = bin_idx_offset + (it->second);
          bitmap_index_.bitmaps[bin_idx].add(j);
        } else {
          assert(false);
        }
      }
    } else if (options_.attr_types[i] == AttrType::CONTINUOUS) {
      const vector<double>& cur_attr_buf = get<vector<double>>(attr_buf_[i]);
      vector<double>& binning =
          get<vector<double>>(bitmap_index_.binning_policy[i]);
      for (uint32_t j = 0; j < cur_attr_buf.size(); ++j) {
        auto it =
            std::upper_bound(binning.begin(), binning.end(), cur_attr_buf[j]);
        uint32_t idx = std::distance(binning.begin(), it);
        uint32_t local_bin_idx =
            (idx == 0) ? 0 : static_cast<uint32_t>(idx - 1);
        if (local_bin_idx >= bitmap_index_.bitmap_nums[i])
          local_bin_idx = bitmap_index_.bitmap_nums[i] - 1;

        uint32_t bin_idx = bin_idx_offset + local_bin_idx;
        bitmap_index_.bitmaps[bin_idx].add(j);
      }
    } else {
      assert(false);
    }
    bin_idx_offset += bitmap_index_.bitmap_nums[i];
  }
}

Status SABIBuilder::Finish(Slice* index_contents) {
  // 1. Determine Binning Policy
  SetBinningPolicy();

  // 2. Calculate bitmap index by buffered attr data & binning policy
  CalculateBitmapIndex();

  // 3. Make final index blob
  // 3-1. Add bitmap indexes
  // Add tombstone bitmap as the last bitmap
  bitmap_index_.bitmaps.push_back(bitmap_index_.tombstone_bitmap);
  vector<uint32_t> bitmap_offsets;
  bitmap_offsets.push_back(index_blob_.size());
  for (uint32_t i = 0; i < bitmap_index_.bitmaps.size(); ++i) {
    Roaring& r = bitmap_index_.bitmaps[i];
    r.runOptimize();
    bitmap_offsets.push_back(bitmap_offsets.back() + r.getFrozenSizeInBytes());
  }
  index_blob_.resize(bitmap_offsets.back());
  for (uint32_t i = 0; i < bitmap_index_.bitmaps.size(); ++i) {
    Roaring& r = bitmap_index_.bitmaps[i];
    r.writeFrozen(index_blob_.data() + bitmap_offsets[i]);
  }
  //  total_offset_table_size = offsets.size() * sizeof(uint32_t);

  // 3-2. Add bitmap index offset
  uint32_t bitmaps_offset_offset = index_blob_.size();
  PutFixed32(&index_blob_, bitmap_offsets.size());
  for (uint32_t& oi : bitmap_offsets) PutFixed32(&index_blob_, oi);

  // 3-3. Add bitmap binning policies
  vector<uint32_t> binning_policy_offset;
  binning_policy_offset.push_back(index_blob_.size());
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    // Add bin count
    PutFixed32(&index_blob_, bitmap_index_.bitmap_nums[i]);
    if (options_.attr_types[i] == AttrType::CATEGORICAL) {
      vector<pair<string, uint32_t>>& cur_binning_policy =
          std::get<vector<pair<string, uint32_t>>>(
              bitmap_index_.binning_policy[i]);
      PutFixed32(&index_blob_,
                 cur_binning_policy.size());  // policy entry count
      for (auto& bi : cur_binning_policy) {
        PutLengthPrefixedSlice(&index_blob_, bi.first);
        PutFixed32(&index_blob_, bi.second);
      }
    } else if (options_.attr_types[i] == AttrType::CONTINUOUS) {
      vector<double>& cur_binning_policy =
          std::get<vector<double>>(bitmap_index_.binning_policy[i]);
      PutFixed32(&index_blob_,
                 cur_binning_policy.size());  // policy entry count
      for (auto& bi : cur_binning_policy) {
        uint64_t val_encoded;
        // Prevent implicit double->uint64_t cast
        std::memcpy(&val_encoded, &bi, sizeof(double));
        PutFixed64(&index_blob_, val_encoded);
      }
    } else {
      assert(false);
    }
    binning_policy_offset.push_back(index_blob_.size());
  }

  // 3-4. Add bitmap binning policy offset
  uint32_t binning_policy_offset_offset = index_blob_.size();
  PutFixed32(&index_blob_, binning_policy_offset.size());
  for (uint32_t& oi : binning_policy_offset) PutFixed32(&index_blob_, oi);

  // 3-5. Add footer
  PutFixed32(&index_blob_, index_entries_cnt_);
  PutFixed32(&index_blob_, bitmaps_offset_offset);
  PutFixed32(&index_blob_, binning_policy_offset_offset);

  *index_contents = Slice(index_blob_);
  // Dump(); // for test only.
  return Status::OK();
}

void SABIBuilder::Dump() {
  cout << "==== SABI Dump ====\n";
  cout << "total_data_entries_size_uncomp_: " << total_data_entries_size_uncomp_
       << "\n";
  cout << "data_entries_cnt_: " << data_entries_cnt_ << "\n";
  cout << "index_entries_cnt_: " << index_entries_cnt_ << "\n";
  cout << "bitmap count: " << bitmap_index_.bitmaps.size() << "\n";
  cout << "bitmap_nums: \n";
  for (uint32_t i = 0; i < bitmap_index_.bitmap_nums.size(); ++i)
    cout << "\t" << bitmap_index_.bitmap_nums[i] << ", ";
  cout << "\n";
}

}  // namespace bit_lsm