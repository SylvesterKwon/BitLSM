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
                         std::unique_ptr<AttrExtractor> extractor)
    : schema_(std::move(schema)),
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
  uint32_t target_total_bitmaps_cnt =
      (uint32_t)(schema_.attr_num() / schema_.rho);

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

// Bin thresholds for one ORDERED attribute: cut the sorted values into
// `bitmap_nums[i]` runs of as near equal row mass as the values allow.
//
// Equal mass is what minimises expected candidate waste. A range query reads a
// contiguous run of bins, and only the two end bins can contribute rows the
// predicate rejects -- everything between them is fully inside the range. If
// query endpoints follow the data, the endpoint lands in bin j with probability
// proportional to its mass and wastes about half of it, so expected waste goes
// as sum(mass^2), minimised when the masses are level.
//
// The cut points are gaps between adjacent values, never values themselves.
// That is the whole difference from asking a t-digest for quantiles: a digest
// answers with interpolated positions, which land beside a value rather than on
// it and repeat on a heavy one. Both hurt. A threshold beside a value puts a
// strict `< value` comparand inside the bin holding every row with that value
// instead of on its edge, so the bin cannot be skipped; a repeated threshold
// spends budget on a bin that can never hold anything. Cutting between values
// makes both impossible by construction, and it is cheaper: the sort is already
// paid for by the distinct count, leaving one linear pass.
void SABIBuilder::SetOrderedPropertyBinningPolicy(uint32_t i) {
  const uint32_t bins = bitmap_index_.bitmap_nums[i];
  const auto& v = get<vector<uint64_t>>(attr_buf_[i]);

  vector<uint64_t> boundaries;
  boundaries.reserve(bins + 1);

  if (v.empty()) {
    distinct_cnts_[i] = 0;
    bitmap_index_.binning_policy[i].emplace<vector<uint64_t>>(bins + 1, 0);
    return;
  }

  // The distinct count (v6 directory field) needs this sorted anyway, and the
  // sweep below reads the same array as (value, run length) pairs.
  vector<uint64_t> sorted(v);
  std::sort(sorted.begin(), sorted.end());

  uint64_t distinct = 0;
  uint64_t remaining_mass = sorted.size();
  uint32_t remaining_bins = bins;
  uint64_t cur = 0;  // mass accumulated into the bin being built
  boundaries.push_back(sorted.front());

  for (size_t a = 0; a < sorted.size();) {
    size_t b = a;
    while (b < sorted.size() && sorted[b] == sorted[a]) ++b;
    const uint64_t run = b - a;
    ++distinct;

    // The target is re-derived from what is left, so closing a bin light or
    // heavy is absorbed by the bins after it instead of accumulating.
    if (cur > 0 && remaining_bins > 1) {
      const double target =
          static_cast<double>(remaining_mass) / remaining_bins;
      // Close ahead of a value heavy enough to fill a bin by itself, so it
      // does not drag its lighter neighbour along; otherwise close on
      // whichever side of the target leaves this bin nearer to it.
      const bool own_bin = static_cast<double>(run) >= target;
      const bool nearer_before =
          std::abs(static_cast<double>(cur) - target) <=
          std::abs(static_cast<double>(cur + run) - target);
      if (own_bin || nearer_before) {
        boundaries.push_back(sorted[a]);
        remaining_mass -= cur;
        --remaining_bins;
        cur = 0;
      }
    }
    cur += run;
    a = b;
  }
  distinct_cnts_[i] = distinct;

  // Fewer distinct values than bins leaves thresholds over. Repeat the maximum:
  // CalculateBitmapIndex clamps a row past the last threshold into the final
  // bin and SABITableIterator's case B resolves a lookup there the same way, so
  // the spare bins sit empty between them rather than swallowing the maximum.
  // Pinning the ends to the exact data bounds also keeps min/max pruning and
  // the virtual-bin -1 path exact.
  while (boundaries.size() < static_cast<size_t>(bins) + 1)
    boundaries.push_back(sorted.back());
  boundaries.front() = sorted.front();
  boundaries.back() = sorted.back();

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

  // 3. Make final index blob (v7 layout; see sabi.h)
  // 3-1. Add frozen bitmaps, tombstone bitmap last. v7: pad every start to a
  // 32-byte boundary (resize() zero-fills the gaps) and record exact sizes
  // and cardinalities for the directory.
  bitmap_index_.bitmaps.push_back(bitmap_index_.tombstone_bitmap);
  const auto align32 = [](uint32_t off) { return (off + 31u) & ~31u; };
  vector<uint32_t> bitmap_offsets;
  vector<uint32_t> bitmap_sizes(bitmap_index_.bitmaps.size());
  vector<uint32_t> bin_cardinalities(bitmap_index_.bitmaps.size());
  bitmap_offsets.push_back(align32(index_blob_.size()));
  for (uint32_t i = 0; i < bitmap_index_.bitmaps.size(); ++i) {
    Roaring& r = bitmap_index_.bitmaps[i];
    r.runOptimize();
    bitmap_sizes[i] = r.getFrozenSizeInBytes();
    bin_cardinalities[i] = static_cast<uint32_t>(r.cardinality());
    bitmap_offsets.push_back(align32(bitmap_offsets.back() + bitmap_sizes[i]));
  }
  last_finish_bitmap_sizes_ = bitmap_sizes;  // test-only ground truth
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
  // v7: per-bin cardinalities (tombstone last) and exact frozen sizes; the
  // padded offsets below no longer encode sizes by difference.
  for (uint32_t c : bin_cardinalities) PutFixed32(&index_blob_, c);
  for (uint32_t sz : bitmap_sizes) PutFixed32(&index_blob_, sz);
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