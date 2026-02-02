#include "util/coding.h"
#include <charconv>
#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <iostream>
#include <queue>
#include <sabi.h>
#include <sys/types.h>

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace bitmap_index {
// ========================================================================
// SABIBuilder Implementation
// ========================================================================
Slice SABIBuilder::AddIndexEntry(const Slice& last_key_in_current_block,
                                 const Slice* first_key_in_next_block,
                                 const BlockHandle& block_handle,
                                 string* separator_scratch) {
  // Add table KVPairs prefix count
  PutFixed32(&index_blob_, data_entries_cnt_);
  ++index_entries_cnt_;
  return last_key_in_current_block;
}

void SABIBuilder::OnKeyAdded(const Slice& key, ValueType type,
                             const Slice& value) {
  // 1. Buffer original SK data
  Slice v = value;
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    Slice res;
    GetLengthPrefixedSlice(&v, &res);
    sk_buf_[i].push_back(res.ToString());
  }

  // 2. Calculate statistics
  ++data_entries_cnt_;
  total_data_entries_size_uncomp_ += value.size();
  total_data_entries_size_uncomp_ += key.size();
}

void SABIBuilder::SetBinningPolicy() {
  // 1. Set target total bitmap index number
  // (Total Bitmap Size)
  // = (total_bitmap_index_cnt) * (N/8)
  // = (total_data_entries_size) * ρ
  uint32_t target_total_bitmap_index_cnt =
      (double)(total_data_entries_size_uncomp_ * 8) * options_.rho /
      index_entries_cnt_;

  // 2. Set # of bitmaps for each SK
  vector<map<string_view, uint32_t>> buf_map(
      options_.sk_num); // map for counting each sk's value
  uint32_t cardinality_ub =
      target_total_bitmap_index_cnt; // theoretically max bins for one SK
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    for (uint32_t j = 0; j < data_entries_cnt_; ++j) {
      buf_map[i][sk_buf_[i][j]]++;
      // Optimization: keep map light when continuous property
      // It is inevitable to keep all value's count when categorical property
      if (options_.sk_types[i] == SKType::CONTINUOUS &&
          buf_map[i].size() >= cardinality_ub)
        break;
    }
  }

  // Weight (importance) vector of SKs.
  // TODO(TASK-85): 읽기 쿼리 집계해서 계산하도록 수정 (현재는 동등하게
  // 들어온다고 가정하고 개발)
  vector<double> query_weight_vector(sk_buf_.size(), 1.0);
  int32_t remaining_budget = target_total_bitmap_index_cnt;
  priority_queue<pair<double, uint32_t>> pq; // {diminishing returns, SK idx}

  for (uint32_t i = 0; i < bitmap_index_nums_.size(); ++i) {
    // Allocate at least 1 bin (prevent divide by zero)
    bitmap_index_nums_[i] = 1;
    remaining_budget--;
    total_bitmap_index_num_++;
    // Delta cost = - N / b_i * (b_i + 1)
    pq.push({query_weight_vector[i] /
                 (bitmap_index_nums_[i] * (bitmap_index_nums_[i] + 1)),
             i});
  }

  while (remaining_budget > 0 && pq.size() > 0) {
    auto [_, idx] = pq.top();
    pq.pop();
    if (buf_map[idx].size() <= bitmap_index_nums_[idx]) {
      continue;
    }
    bitmap_index_nums_[idx]++;
    remaining_budget--;
    total_bitmap_index_num_++;
    pq.push({query_weight_vector[idx] /
                 (bitmap_index_nums_[idx] * (bitmap_index_nums_[idx] + 1)),
             idx});
  }
  bitmap_index_.resize(total_bitmap_index_num_);

  // 3. Set binning boundaries for each SK

  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      // 3-A. Categorical property Binning
      SetCategoricalPropertyBinningPolicy(i, buf_map);
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      // 3-B. Continuous property Binning
      SetContinuousPropertyBinningPolicy(i, buf_map);
    } else {
      assert(false);
    }
  }
}

void SABIBuilder::SetCategoricalPropertyBinningPolicy(
    uint32_t i, vector<map<string_view, uint32_t>>& buf_map) {
  priority_queue<pair<uint32_t, uint32_t>, vector<pair<uint32_t, uint32_t>>,
                 greater<pair<uint32_t, uint32_t>>>
      min_bin_pq;
  vector<pair<string_view, uint32_t>> sorted_items; // sorted by cnt
  for (const auto& [val, cnt] : buf_map[i]) {
    sorted_items.push_back({val, cnt});
  }
  std::sort(sorted_items.begin(), sorted_items.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  vector<pair<string, uint32_t>> binning;
  binning.reserve(sorted_items.size());
  for (uint32_t j = 0; j < bitmap_index_nums_[i]; ++j)
    min_bin_pq.push({0, j});
  for (auto& [val, cnt] : sorted_items) {
    auto [cur_bin_cnt, bin_idx] = min_bin_pq.top();
    min_bin_pq.pop();
    binning.push_back({string(val), bin_idx});
    min_bin_pq.push({cur_bin_cnt + cnt, bin_idx});
  }
  sort(binning.begin(), binning.end());
  binning_policy[i] = std::move(binning);
}

void SABIBuilder::SetContinuousPropertyBinningPolicy(
    uint32_t i, std::vector<std::map<std::string_view, uint32_t>>& buf_map) {
  folly::TDigest digest(
      1000); // TODO: Make this configurable / driven with some fomular
  vector<double> v(sk_buf_[i].size());
  transform(sk_buf_[i].begin(), sk_buf_[i].end(), v.begin(),
            [](const std::string& s) {
              double res = 0.0;
              auto result = from_chars(s.data(), s.data() + s.size(), res);
              if (result.ec != std::errc()) {
                cerr << "Error while transform sk_buf_ to double vector\n";
                assert(false);
              }
              return res;
            });
  digest = digest.merge(folly::Range<const double*>(v.data(), v.size()));
  // N+1 boundary points
  vector<double> boundaries(bitmap_index_nums_[i] + 1);
  for (uint32_t j = 0; j <= bitmap_index_nums_[i]; ++j) {
    double quantile = (double)j / (double)bitmap_index_nums_[i];
    boundaries[j] = digest.estimateQuantile(quantile);
  }
  binning_policy[i] = std::move(boundaries);
}

void SABIBuilder::CalculateBitmapIndex() {
  uint32_t bin_idx_offset = 0;
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      vector<pair<string, uint32_t>>& binning =
          std::get<vector<pair<string, uint32_t>>>(binning_policy[i]);
      for (uint32_t j = 0; j < sk_buf_[i].size(); ++j) {
        const string& key = sk_buf_[i][j];

        auto it = std::lower_bound(
            binning.begin(), binning.end(), key,
            [](const pair<string, uint32_t>& element, const string& val) {
              return element.first < val;
            });
        if (it != binning.end() && it->first == key) {
          uint32_t bin_idx = bin_idx_offset + (it->second);
          bitmap_index_[bin_idx].add(j);
        } else {
          assert(false);
        }
      }
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      vector<double>& binning = std::get<vector<double>>(binning_policy[i]);
      for (uint32_t j = 0; j < sk_buf_[i].size(); ++j) {
        const string& key = sk_buf_[i][j];
        double key_value = 0.0;
        auto res =
            std::from_chars(key.data(), key.data() + key.size(), key_value);
        if (res.ec != std::errc())
          assert(false && "Invalid double string in buffer");

        auto it = std::upper_bound(binning.begin(), binning.end(), key_value);
        uint32_t idx = std::distance(binning.begin(), it);
        uint32_t local_bin_idx =
            (idx == 0) ? 0 : static_cast<uint32_t>(idx - 1);
        if (local_bin_idx >= bitmap_index_nums_[i])
          local_bin_idx = bitmap_index_nums_[i] - 1;

        uint32_t bin_idx = bin_idx_offset + local_bin_idx;
        bitmap_index_[bin_idx].add(j);
      }
    } else {
      assert(false);
    }
    bin_idx_offset += bitmap_index_nums_[i];
  }
}

Status SABIBuilder::Finish(Slice* index_contents) {
  // 1. Determine Binning Policy
  SetBinningPolicy();

  // 2. Calculate bitmap index by buffered SK data & binning policy
  CalculateBitmapIndex();

  // 3. Make final index blob
  // 3-1. Add bitmap indexes
  vector<uint32_t> bitmap_index_offsets;
  bitmap_index_offsets.push_back(index_blob_.size());
  for (uint32_t i = 0; i < bitmap_index_.size(); ++i) {
    Roaring& r = bitmap_index_[i];
    r.runOptimize();
    bitmap_index_offsets.push_back(bitmap_index_offsets.back() +
                                   r.getFrozenSizeInBytes());
  }
  index_blob_.resize(bitmap_index_offsets.back());
  for (uint32_t i = 0; i < bitmap_index_.size(); ++i) {
    Roaring& r = bitmap_index_[i];
    r.writeFrozen(index_blob_.data() + bitmap_index_offsets[i]);
  }
  //  total_offset_table_size = offsets.size() * sizeof(uint32_t);

  // 3-2. Add bitmap index offset
  uint32_t bitmap_index_offset_offset = index_blob_.size();
  PutFixed32(&index_blob_, bitmap_index_offsets.size());
  for (uint32_t& oi : bitmap_index_offsets)
    PutFixed32(&index_blob_, oi);

  // 3-3. Add bitmap binning policies
  vector<uint32_t> binning_policy_offset;
  binning_policy_offset.push_back(index_blob_.size());
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    PutFixed32(&index_blob_,
               static_cast<uint32_t>(options_.sk_types[i])); // Mark SKType
    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      vector<pair<string, uint32_t>>& binning =
          std::get<vector<pair<string, uint32_t>>>(binning_policy[i]);
      PutFixed32(&index_blob_, binning.size());
      for (auto& bi : binning) {
        PutLengthPrefixedSlice(&index_blob_, bi.first);
        PutFixed32(&index_blob_, bi.second);
      }
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      vector<double>& binning = std::get<vector<double>>(binning_policy[i]);
      PutFixed32(&index_blob_, binning.size());
      for (auto& bi : binning)
        PutFixed64(&index_blob_, bi);
    } else {
      assert(false);
    }
    binning_policy_offset.push_back(index_blob_.size());
  }

  // 3-4. Add bitmap binning policy offset
  uint32_t binning_policy_offset_offset = index_blob_.size();
  PutFixed32(&index_blob_, binning_policy_offset.size());
  for (uint32_t& oi : binning_policy_offset)
    PutFixed32(&index_blob_, oi);

  // 3-5. Add footer
  PutFixed32(&index_blob_, index_entries_cnt_);
  PutFixed32(&index_blob_, bitmap_index_offset_offset);
  PutFixed32(&index_blob_, binning_policy_offset_offset);

  *index_contents = Slice(index_blob_);
  return Status::OK();
}
} // namespace bitmap_index
