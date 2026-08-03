#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <sabi.h>
#include <sys/types.h>

#include <iostream>
#include <queue>

#include "util/coding.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace bit_lsm {
// ========================================================================
// SABIBuilder Implementation
// ========================================================================
SABIBuilder::SABIBuilder(SABISchema schema,
                         std::unique_ptr<AttrExtractor> extractor,
                         uint32_t level_d)
    : schema_(std::move(schema)),
      level_d_(level_d),
      extractor_(std::move(extractor)),
      scratch_(schema_.attr_num()) {
  bitmap_index_.bitmap_nums.resize(schema_.attr_num(), 0);
  bitmap_index_.binning_policy.resize(schema_.attr_num());
  distinct_cnts_.resize(schema_.attr_num(), 0);
  attr_null_rows_.resize(schema_.attr_num());
  attr_buf_.reserve(schema_.attr_num());
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    if (schema_.roles[i] == AttrRole::ORDERED) {
      attr_buf_.push_back(vector<uint64_t>());
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

  // 2. Buffer original attr data. Buffers are dense (data rows only): NULL
  // and tombstone rows are recorded in their bitmaps and push nothing, so
  // every downstream statistic can scan a buffer front to back with no
  // exclusion logic. Row-id alignment is reconstructed once, in
  // CalculateBitmapIndex.
  if (is_value) {
    std::string_view buffer(value.data(), value.size());
    extractor_->ExtractAll(std::string_view(key.data(), key.size()), buffer,
                           scratch_.data());
    for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
      const EncodedAttr& attr_val = scratch_[i];
      if (holds_alternative<monostate>(attr_val)) {
        attr_null_rows_[i].add(data_entries_cnt_);
      } else if (schema_.roles[i] == AttrRole::ORDERED) {
        get<vector<uint64_t>>(attr_buf_[i]).push_back(get<uint64_t>(attr_val));
      } else {
        get<CatAttrBuf>(attr_buf_[i]).Intern(get<string_view>(attr_val));
      }
    }
  } else {
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
  // Levels above the deepest one get a gamma-decayed budget (no-op at
  // gamma = 1.0).
  double rho_eff = EffectiveRho(schema_.rho, schema_.gamma, level_d_);
  uint32_t target_total_bitmaps_cnt = (uint32_t)(schema_.attr_num() / rho_eff);

  // 2. Set # of bitmaps for each attr
  vector<uint32_t> cardinality(schema_.attr_num(), 0);
  uint32_t cardinality_ub =
      target_total_bitmaps_cnt;  // theoretically max bins for one attr
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    if (schema_.roles[i] == AttrRole::UNORDERED) {
      cardinality[i] = get<CatAttrBuf>(attr_buf_[i]).value_by_id.size();
    } else {
      const auto& okey_vec = get<vector<uint64_t>>(attr_buf_[i]);
      unordered_set<uint64_t> unique_vals;
      for (uint64_t val : okey_vec) {
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
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    if (schema_.roles[i] == AttrRole::UNORDERED) {
      // 3-A. Unordered property Binning
      SetUnorderedPropertyBinningPolicy(i);
    } else if (schema_.roles[i] == AttrRole::ORDERED) {
      // 3-B. Ordered property Binning
      SetOrderedPropertyBinningPolicy(i);
    } else {
      assert(false);
    }
  }
}

void SABIBuilder::SetUnorderedPropertyBinningPolicy(uint32_t i) {
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
  // v6 directory field: the interning table is exactly the distinct set.
  distinct_cnts_[i] = cat.value_by_id.size();
}

void SABIBuilder::SetOrderedPropertyBinningPolicy(uint32_t i) {
  folly::TDigest digest(bitmap_index_.bitmap_nums[i] * 5);

  // The buffer is dense (data rows only), so the bounds and quantiles are
  // plain scans. Exact data bounds first: the t-digest projection is shifted
  // by min_okey so the double mantissa covers the okey span, not the
  // absolute magnitude (see OkeyToTDigest).
  const auto& v = get<vector<uint64_t>>(attr_buf_[i]);
  uint64_t min_okey = UINT64_MAX, max_okey = 0;
  for (uint64_t okey : v) {
    min_okey = std::min(min_okey, okey);
    max_okey = std::max(max_okey, okey);
  }

  vector<double> proj;
  proj.reserve(v.size());
  for (uint64_t okey : v) proj.push_back(OkeyToTDigest(okey, min_okey));
  digest = digest.merge(folly::Range<const double*>(proj.data(), proj.size()));

  // v6 directory field: exact distinct okeys, for the estimator's equality
  // floor (sparse integer domains smear point mass into value holes). A
  // sorted copy costs O(n log n) at flush, same order as the t-digest merge
  // above; no extra hash table peak.
  {
    vector<uint64_t> uniq(v);
    std::sort(uniq.begin(), uniq.end());
    distinct_cnts_[i] = std::unique(uniq.begin(), uniq.end()) - uniq.begin();
  }

  // N+1 okey boundary thresholds.
  vector<uint64_t> boundaries(bitmap_index_.bitmap_nums[i] + 1);
  for (uint32_t j = 0; j <= bitmap_index_.bitmap_nums[i]; ++j) {
    double quantile = (double)j / (double)bitmap_index_.bitmap_nums[i];
    boundaries[j] =
        TDigestBoundaryToOkey(digest.estimateQuantile(quantile), min_okey);
  }

  // Pin the outer thresholds to the exact data bounds: min/max pruning and
  // the virtual-bin -1 path treat them as [min, max] of the data, and for
  // spans >= 2^53 the shifted round trip can still round past the true
  // bounds, turning EQ(min)/EQ(max) into false negatives. Interior
  // boundaries are shared thresholds, so their rounding is harmless; clamp
  // into the pinned range and keep them monotone.
  if (!proj.empty()) {
    boundaries.front() = min_okey;
    boundaries.back() = max_okey;
  }
  for (uint32_t j = 1; j < boundaries.size(); ++j) {
    boundaries[j] = std::min(boundaries[j], boundaries.back());
    if (boundaries[j] < boundaries[j - 1]) boundaries[j] = boundaries[j - 1];
  }
  bitmap_index_.binning_policy[i] = std::move(boundaries);
}

void SABIBuilder::CalculateBitmapIndex() {
  uint32_t bin_idx_offset = 0;
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    // The attr buffer is dense (data rows only), so this is the one place
    // that re-aligns buffer entries with row ids: rows in NULL/tombstone
    // bitmaps pushed nothing, and skipping exactly those ids while walking
    // j keeps the dense cursor k in lockstep with the row id.
    roaring::Roaring excluded =
        attr_null_rows_[i] | bitmap_index_.tombstone_bitmap;
    vector<uint32_t> skip_ids(excluded.cardinality());
    excluded.toUint32Array(skip_ids.data());
    size_t s = 0, k = 0;

    // Row ids are monotonic per bin, so a bulk context per bin lets
    // CRoaring skip the container lookup on nearly every add.
    vector<roaring::BulkContext> bin_ctxs(bitmap_index_.bitmap_nums[i]);
    if (schema_.roles[i] == AttrRole::UNORDERED) {
      const CatAttrBuf& cat = get<CatAttrBuf>(attr_buf_[i]);
      for (uint32_t j = 0; j < data_entries_cnt_; ++j) {
        if (s < skip_ids.size() && skip_ids[s] == j) {
          ++s;
          continue;
        }
        uint32_t local_bin = cat.bin_by_id[cat.row_ids[k++]];
        bitmap_index_.bitmaps[bin_idx_offset + local_bin].addBulk(
            bin_ctxs[local_bin], j);
      }
      assert(k == cat.row_ids.size());
    } else if (schema_.roles[i] == AttrRole::ORDERED) {
      const vector<uint64_t>& cur_attr_buf =
          get<vector<uint64_t>>(attr_buf_[i]);
      vector<uint64_t>& binning =
          get<vector<uint64_t>>(bitmap_index_.binning_policy[i]);
      for (uint32_t j = 0; j < data_entries_cnt_; ++j) {
        if (s < skip_ids.size() && skip_ids[s] == j) {
          ++s;
          continue;
        }
        auto it =
            std::upper_bound(binning.begin(), binning.end(), cur_attr_buf[k++]);
        uint32_t idx = std::distance(binning.begin(), it);
        uint32_t local_bin_idx =
            (idx == 0) ? 0 : static_cast<uint32_t>(idx - 1);
        if (local_bin_idx >= bitmap_index_.bitmap_nums[i])
          local_bin_idx = bitmap_index_.bitmap_nums[i] - 1;

        bitmap_index_.bitmaps[bin_idx_offset + local_bin_idx].addBulk(
            bin_ctxs[local_bin_idx], j);
      }
      assert(k == cur_attr_buf.size());
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

  // 3. Make final index blob (v5 layout; see sabi.h)
  // 3-1. Add frozen bitmaps, tombstone bitmap last
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

  // 3-2. Add binning policies. ORDERED bodies are headerless: the boundary
  // count is bin_count+1, and bin_count lives in the directory.
  vector<uint32_t> policy_offsets;
  policy_offsets.push_back(index_blob_.size());
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    if (schema_.roles[i] == AttrRole::UNORDERED) {
      vector<pair<string, uint32_t>>& cur_binning_policy =
          std::get<vector<pair<string, uint32_t>>>(
              bitmap_index_.binning_policy[i]);
      PutFixed32(&index_blob_,
                 cur_binning_policy.size());  // policy entry count
      for (auto& bi : cur_binning_policy) {
        PutLengthPrefixedSlice(&index_blob_, bi.first);
        PutFixed32(&index_blob_, bi.second);
      }
    } else if (schema_.roles[i] == AttrRole::ORDERED) {
      vector<uint64_t>& cur_binning_policy =
          std::get<vector<uint64_t>>(bitmap_index_.binning_policy[i]);
      for (uint64_t bi : cur_binning_policy) PutFixed64(&index_blob_, bi);
    } else {
      assert(false);
    }
    policy_offsets.push_back(index_blob_.size());
  }

  // 3-3. Add directory: everything a reader must know before touching the
  // body, parsed forward from attr_num. Persisted roles make the blob
  // self-describing, so opening an SST needs no schema binding.
  uint32_t directory_off = index_blob_.size();
  PutFixed32(&index_blob_, schema_.attr_num());
  for (AttrRole role : schema_.roles)
    index_blob_.push_back(static_cast<char>(role));
  for (uint32_t bin_num : bitmap_index_.bitmap_nums)
    PutFixed32(&index_blob_, bin_num);
  PutFixed32(&index_blob_, index_entries_cnt_);
  // v6: per-attr exact distinct counts (see the format comment in sabi.h).
  for (uint64_t d : distinct_cnts_) PutFixed64(&index_blob_, d);
  for (uint32_t& oi : policy_offsets) PutFixed32(&index_blob_, oi);
  for (uint32_t& oi : bitmap_offsets) PutFixed32(&index_blob_, oi);

  // 3-4. Add fixed footer (validated by SABIFactory::NewReader)
  PutFixed32(&index_blob_, directory_off);
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