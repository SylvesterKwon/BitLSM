#include <sabi.h>
#include <sys/types.h>

#include <iostream>
#include <numeric>
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
    if (schema_.index_types[i] == IndexType::kRange) {
      attr_buf_.push_back(RangeAttrBuf());
    } else {
      attr_buf_.push_back(EqualityAttrBuf());
    }
  }
};

void SABIBuilder::EqualityAttrBuf::Intern(string_view value) {
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

void SABIBuilder::EqualityAttrBuf::Grow() {
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
      } else if (schema_.index_types[i] == IndexType::kRange) {
        get<RangeAttrBuf>(attr_buf_[i]).Push(get<string_view>(attr_val));
      } else {
        get<EqualityAttrBuf>(attr_buf_[i]).Intern(get<string_view>(attr_val));
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
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    if (schema_.index_types[i] == IndexType::kEquality) {
      cardinality[i] = get<EqualityAttrBuf>(attr_buf_[i]).value_by_id.size();
    } else {
      RangeAttrBuf& buf = get<RangeAttrBuf>(attr_buf_[i]);
      buf.Sort();
      cardinality[i] = static_cast<uint32_t>(
          std::min<uint64_t>(buf.distinct, target_total_bitmaps_cnt));
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
    if (schema_.index_types[i] == IndexType::kEquality) {
      // 3-A. Unordered property Binning
      SetEqualityBinningPolicy(i);
    } else if (schema_.index_types[i] == IndexType::kRange) {
      // 3-B. Ordered property Binning
      SetRangeBinningPolicy(i);
    } else {
      assert(false);
    }
  }
}

void SABIBuilder::SetEqualityBinningPolicy(uint32_t i) {
  EqualityAttrBuf& cat = get<EqualityAttrBuf>(attr_buf_[i]);
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
  // Directory field: the interning table is exactly the distinct set.
  distinct_cnts_[i] = cat.value_by_id.size();
}

void SABIBuilder::RangeAttrBuf::Push(string_view v) {
  const int32_t len = static_cast<int32_t>(v.size());
  if (uniform_len == -2)
    uniform_len = len;
  else if (uniform_len != len)
    uniform_len = -1;
  values.push_back(v);
}

void SABIBuilder::RangeAttrBuf::Sort() {
  const uint32_t n = static_cast<uint32_t>(values.size());
  sorted.resize(n);
  run_start.clear();
  if (uniform_len == static_cast<int32_t>(kOkeyBytes)) {
    // Every value is an okey: sort (okey, row) records contiguously -- the
    // cost of the plain integer sort -- and cut runs on the way out.
    struct Rec {
      uint64_t key;
      uint32_t row;
    };
    vector<Rec> recs(n);
    for (uint32_t i = 0; i < n; ++i) recs[i] = {OkeyFromBytes(values[i]), i};
    std::sort(recs.begin(), recs.end(),
              [](const Rec& x, const Rec& y) { return x.key < y.key; });
    for (uint32_t i = 0; i < n; ++i) {
      sorted[i] = recs[i].row;
      if (i == 0 || recs[i].key != recs[i - 1].key) run_start.push_back(i);
    }
  } else {
    std::iota(sorted.begin(), sorted.end(), 0u);
    std::sort(sorted.begin(), sorted.end(), [this](uint32_t a, uint32_t b) {
      return values[a] < values[b];
    });
    for (uint32_t i = 0; i < n; ++i)
      if (i == 0 || values[sorted[i]] != values[sorted[i - 1]])
        run_start.push_back(i);
  }
  distinct = run_start.size();
  if (n > 0) run_start.push_back(n);
}

// Bin thresholds for one kRange attribute: cut the sorted values into
// `bitmap_nums[i]` runs of as near equal row mass as the values allow.
//
// Equal mass is what minimises expected candidate waste. A range query reads a
// contiguous run of bins, and only the two end bins can contribute rows the
// predicate rejects -- everything between them is fully inside the range. If
// query endpoints follow the data, the endpoint lands in bin j with probability
// proportional to its mass and wastes about half of it, so expected waste goes
// as sum(mass^2), minimised when the masses are level.
//
// The cut points are values that occur, never interpolated positions between
// them. A threshold beside a value would put a strict `< value` comparand
// inside the bin holding every row with that value instead of on its edge, so
// the bin could not be skipped; a repeated threshold would spend budget on a
// bin that can never hold anything. Cutting on run starts makes both
// impossible by construction, in one linear pass over the sorted buffer.
void SABIBuilder::SetRangeBinningPolicy(uint32_t i) {
  const uint32_t bins = bitmap_index_.bitmap_nums[i];
  RangeAttrBuf& buf = get<RangeAttrBuf>(attr_buf_[i]);
  const BytesList& v = buf.values;
  const vector<uint32_t>& sorted = buf.sorted;
  const vector<uint32_t>& runs = buf.run_start;
  distinct_cnts_[i] = buf.distinct;

  BytesList boundaries;
  if (sorted.empty()) {
    for (uint32_t b = 0; b <= bins; ++b) boundaries.push_back("");
    bitmap_index_.binning_policy[i] = std::move(boundaries);
    return;
  }

  const size_t run_cnt = runs.size() - 1;
  uint64_t remaining_mass = sorted.size();
  uint32_t remaining_bins = bins;
  uint64_t cur = 0;  // mass accumulated into the bin being built
  // Every threshold is the value of some run; remembering which run lets the
  // bin assignment below compare run indices instead of bytes.
  vector<uint32_t> boundary_run;
  boundaries.push_back(v[sorted.front()]);
  boundary_run.push_back(0);

  for (size_t r = 0; r < run_cnt; ++r) {
    const uint64_t run = runs[r + 1] - runs[r];

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
        boundaries.push_back(v[sorted[runs[r]]]);
        boundary_run.push_back(static_cast<uint32_t>(r));
        remaining_mass -= cur;
        --remaining_bins;
        cur = 0;
      }
    }
    cur += run;
  }

  // The sweep closes at most bins-1 times, so at least one threshold is
  // always left over. Repeat the maximum: rows past the last threshold land
  // in the final bin (below) and SelectBins resolves a lookup there the same
  // way, so spare bins sit empty between them rather than swallowing the
  // maximum, and the ends stay pinned to the exact data bounds for min/max
  // pruning.
  const string max_value(v[sorted.back()]);
  while (boundaries.size() < static_cast<size_t>(bins) + 1) {
    boundaries.push_back(max_value);
    boundary_run.push_back(static_cast<uint32_t>(run_cnt - 1));
  }

  // Bin of each row = (number of boundaries <= value) - 1, clamped -- the
  // same rule SelectBins applies through BytesList::UpperBound. Runs ascend
  // and each threshold is a run's value, so "threshold <= value" is
  // "threshold's run <= this run": one forward cursor over run indices
  // settles every row in O(n) without touching a byte.
  buf.bin_of_row.resize(sorted.size());
  uint32_t passed = 0;  // thresholds at or below the current run
  for (uint32_t r = 0; r < run_cnt; ++r) {
    while (passed < boundary_run.size() && boundary_run[passed] <= r) ++passed;
    uint32_t bin = passed == 0 ? 0 : passed - 1;
    if (bin >= bins) bin = bins - 1;
    for (uint32_t k = runs[r]; k < runs[r + 1]; ++k)
      buf.bin_of_row[sorted[k]] = bin;
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
    if (schema_.index_types[i] == IndexType::kEquality) {
      const EqualityAttrBuf& cat = get<EqualityAttrBuf>(attr_buf_[i]);
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
    } else if (schema_.index_types[i] == IndexType::kRange) {
      const vector<uint32_t>& bin_of_row =
          get<RangeAttrBuf>(attr_buf_[i]).bin_of_row;
      for (uint32_t j = 0; j < data_entries_cnt_; ++j) {
        if (s < skip_ids.size() && skip_ids[s] == j) {
          ++s;
          continue;
        }
        const uint32_t local_bin_idx = bin_of_row[k++];
        bitmap_index_.bitmaps[bin_idx_offset + local_bin_idx].addBulk(
            bin_ctxs[local_bin_idx], j);
      }
      assert(k == bin_of_row.size());
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

  // 3. Make final index blob (v8 layout; see sabi.h)
  // 3-1. Add frozen bitmaps, tombstone bitmap last. Pad every start to a
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

  // 3-2. Add binning policies. kRange bodies are a BytesList of bin_count+1
  // boundaries; kEquality bodies carry their entry count.
  vector<uint32_t> policy_offsets;
  policy_offsets.push_back(index_blob_.size());
  for (uint32_t i = 0; i < schema_.attr_num(); ++i) {
    if (schema_.index_types[i] == IndexType::kEquality) {
      vector<pair<string, uint32_t>>& cur_binning_policy =
          std::get<vector<pair<string, uint32_t>>>(
              bitmap_index_.binning_policy[i]);
      PutFixed32(&index_blob_,
                 cur_binning_policy.size());  // policy entry count
      for (auto& bi : cur_binning_policy) {
        PutLengthPrefixedSlice(&index_blob_, bi.first);
        PutFixed32(&index_blob_, bi.second);
      }
    } else if (schema_.index_types[i] == IndexType::kRange) {
      std::get<BytesList>(bitmap_index_.binning_policy[i])
          .Serialize(&index_blob_);
    } else {
      assert(false);
    }
    policy_offsets.push_back(index_blob_.size());
  }

  // 3-3. Add directory: everything a reader must know before touching the
  // body, parsed forward from attr_num. Persisted index_types make the blob
  // self-describing, so opening an SST needs no schema binding.
  uint32_t directory_off = index_blob_.size();
  PutFixed32(&index_blob_, schema_.attr_num());
  for (IndexType index_type : schema_.index_types)
    index_blob_.push_back(static_cast<char>(index_type));
  for (uint32_t bin_num : bitmap_index_.bitmap_nums)
    PutFixed32(&index_blob_, bin_num);
  PutFixed32(&index_blob_, index_entries_cnt_);
  // Per-attr exact distinct counts.
  for (uint64_t d : distinct_cnts_) PutFixed64(&index_blob_, d);
  // Per-bin cardinalities (tombstone last) and exact frozen sizes (the
  // padded offsets below do not encode sizes by difference).
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