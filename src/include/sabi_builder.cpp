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
      attr_buf_.push_back(CatAttrBuf());
    }
  }
};

void SABIBuilder::CatAttrBuf::Intern(string_view value) {
  uint64_t h = std::hash<string_view>{}(value);
  size_t mask = slot_id_.size() - 1;
  size_t idx = h & mask;
  while (slot_id_[idx] != 0) {
    if (slot_hash_[idx] == h && ValueOf(slot_id_[idx] - 1) == value) {
      uint32_t id = slot_id_[idx] - 1;
      count_by_id[id]++;
      row_ids.push_back(id);
      return;
    }
    idx = (idx + 1) & mask;
  }
  uint32_t id = static_cast<uint32_t>(value_by_id.size());
  value_by_id.push_back({static_cast<uint32_t>(arena.size()),
                         static_cast<uint32_t>(value.size())});
  arena.append(value.data(), value.size());
  count_by_id.push_back(1);
  row_ids.push_back(id);
  slot_hash_[idx] = h;
  slot_id_[idx] = id + 1;
  if (++used_ * 10 >= slot_id_.size() * 7) Grow();
}

void SABIBuilder::CatAttrBuf::Grow() {
  size_t n = slot_id_.size() * 2;
  vector<uint64_t> new_hash(n);
  vector<uint32_t> new_id(n, 0);
  size_t mask = n - 1;
  for (size_t i = 0; i < slot_id_.size(); ++i) {
    if (slot_id_[i] == 0) continue;
    size_t idx = slot_hash_[i] & mask;
    while (new_id[idx] != 0) idx = (idx + 1) & mask;
    new_hash[idx] = slot_hash_[i];
    new_id[idx] = slot_id_[i];
  }
  slot_hash_ = std::move(new_hash);
  slot_id_ = std::move(new_id);
}

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
        get<CatAttrBuf>(attr_buf_[i]).Intern(get<string_view>(attr_val));
      }
    }
  } else {
    // If it's a tombstone, we still need to add a dummy value to the buffer for
    // the sake of simplicity in binning policy calculation.
    for (uint32_t i = 0; i < options_.attr_num; ++i) {
      if (options_.attr_types[i] == AttrType::CONTINUOUS) {
        get<vector<double>>(attr_buf_[i]).push_back(0.0);
      } else {
        get<CatAttrBuf>(attr_buf_[i]).Intern("");
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
  vector<uint32_t> cardinality(options_.attr_num, 0);
  uint32_t cardinality_ub =
      target_total_bitmaps_cnt;  // theoretically max bins for one attr
  for (uint32_t i = 0; i < options_.attr_num; ++i) {
    if (options_.attr_types[i] == AttrType::CATEGORICAL) {
      cardinality[i] = get<CatAttrBuf>(attr_buf_[i]).value_by_id.size();
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
      SetCategoricalPropertyBinningPolicy(i);
    } else if (options_.attr_types[i] == AttrType::CONTINUOUS) {
      // 3-B. Continuous property Binning
      SetContinuousPropertyBinningPolicy(i);
    } else {
      assert(false);
    }
  }
}

void SABIBuilder::SetCategoricalPropertyBinningPolicy(uint32_t i) {
  CatAttrBuf& cat = get<CatAttrBuf>(attr_buf_[i]);
  priority_queue<pair<uint32_t, uint32_t>, vector<pair<uint32_t, uint32_t>>,
                 greater<pair<uint32_t, uint32_t>>>
      min_bin_pq;
  // Distinct value ids sorted by occurrence count (descending)
  vector<uint32_t> sorted_ids(cat.value_by_id.size());
  for (uint32_t id = 0; id < sorted_ids.size(); ++id) sorted_ids[id] = id;
  std::sort(sorted_ids.begin(), sorted_ids.end(),
            [&cat](uint32_t a, uint32_t b) {
              return cat.count_by_id[a] > cat.count_by_id[b];
            });
  vector<pair<string, uint32_t>> binning;
  binning.reserve(sorted_ids.size());
  cat.bin_by_id.resize(sorted_ids.size());
  for (uint32_t j = 0; j < bitmap_index_.bitmap_nums[i]; ++j)
    min_bin_pq.push({0, j});
  for (uint32_t id : sorted_ids) {
    auto [cur_bin_cnt, bin_idx] = min_bin_pq.top();
    min_bin_pq.pop();
    binning.push_back({string(cat.ValueOf(id)), bin_idx});
    cat.bin_by_id[id] = bin_idx;
    min_bin_pq.push({cur_bin_cnt + cat.count_by_id[id], bin_idx});
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
      const CatAttrBuf& cat = get<CatAttrBuf>(attr_buf_[i]);
      // Row ids are monotonic per bin, so a bulk context per bin lets
      // CRoaring skip the container lookup on nearly every add.
      vector<roaring::BulkContext> bin_ctxs(bitmap_index_.bitmap_nums[i]);
      for (uint32_t j = 0; j < cat.row_ids.size(); ++j) {
        uint32_t local_bin = cat.bin_by_id[cat.row_ids[j]];
        bitmap_index_.bitmaps[bin_idx_offset + local_bin].addBulk(
            bin_ctxs[local_bin], j);
      }
    } else if (options_.attr_types[i] == AttrType::CONTINUOUS) {
      const vector<double>& cur_attr_buf = get<vector<double>>(attr_buf_[i]);
      vector<double>& binning =
          get<vector<double>>(bitmap_index_.binning_policy[i]);
      vector<roaring::BulkContext> bin_ctxs(bitmap_index_.bitmap_nums[i]);
      for (uint32_t j = 0; j < cur_attr_buf.size(); ++j) {
        auto it =
            std::upper_bound(binning.begin(), binning.end(), cur_attr_buf[j]);
        uint32_t idx = std::distance(binning.begin(), it);
        uint32_t local_bin_idx =
            (idx == 0) ? 0 : static_cast<uint32_t>(idx - 1);
        if (local_bin_idx >= bitmap_index_.bitmap_nums[i])
          local_bin_idx = bitmap_index_.bitmap_nums[i] - 1;

        bitmap_index_.bitmaps[bin_idx_offset + local_bin_idx].addBulk(
            bin_ctxs[local_bin_idx], j);
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

  // 3-6. Version footer (validated by SABIFactory::NewReader)
  PutFixed32(&index_blob_, kBitLSMFormatVersion);
  PutFixed32(&index_blob_, kSABIFooterMagic);

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