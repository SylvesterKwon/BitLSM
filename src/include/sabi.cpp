#include "util/coding.h"
#include "util/coding_lean.h"
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

// TODO: Block random iteration 로직 가져와서 통합하기
namespace bitmap_index {
// ========================================================================
// SABIBuilder Implementation
// ========================================================================
Slice SABIBuilder::AddIndexEntry(const Slice& last_key_in_current_block,
                                 const Slice* first_key_in_next_block,
                                 const BlockHandle& block_handle,
                                 string* separator_scratch) {
  // Add table KVPairs prefix count
  PutFixed32(&final_index_blob_, data_entries_cnt_);
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
  vector<uint32_t> offsets;
  offsets.push_back(final_index_blob_.size());
  for (uint32_t i = 0; i < bitmap_index_.size(); ++i) {
    Roaring& r = bitmap_index_[i];
    r.runOptimize();
    r.writeFrozen(final_index_blob_.data());
    offsets.push_back(offsets.back() + r.getFrozenSizeInBytes());
  }
  //  total_offset_table_size = offsets.size() * sizeof(uint32_t);

  // 3-2. Add bitmap index offset
  uint32_t bitmap_index_offset_offset = final_index_blob_.size();
  for (uint32_t& oi : offsets)
    PutFixed32(&final_index_blob_, oi);

  // 3-3. Add bitmap binning policies
  /*
초안: 정책리스트 offset vector 사용해서 개별적으로 읽을 수 있도록 하기
경계 1 ...
경계 2 ...
...
경계 k ...
offset 1 2 ... k
TODO: 각 속성별 비트맵 인덱스의 offset 리스트도 인코딩해야될듯, 지금
*/
  // 3-4. Add footer

  final_index_blob_.resize(base_offset + total_roaring_size);

  // 4. Write Footer
  PutFixed32(&final_index_blob_, index_entries_cnt_);
  uint32_t bitmap_cnt = bitmap_index_.size();
  PutFixed32(&final_index_blob_, bitmap_cnt);

  *index_contents = Slice(final_index_blob_);
  return Status::OK();
}

// ========================================================================
// SABIIterator Implementation
// ========================================================================

SABIIterator::SABIIterator(const SABIReader* reader) : reader_(reader) {}

void SABIIterator::Prepare(const ScanOptions scan_opts[], size_t num_opts) {
  // 1. ScanOption 확인
  assert(num_opts == 1 && scan_opts[0].property_bag);
  range_ = &scan_opts[0].range;
  auto it = scan_opts[0].property_bag->find("qc");
  assert(it != scan_opts[0].property_bag->end());
  qc = it->second;

  // 2. bitset filtering 사용하여 방문 필요한 key 정리
  // TODO: 이후에 복합 쿼리 들어오면 이곳에서 필요한 모든 전처리 (bitset
  // 조합등) 진행할 것

  // TODO(TASK-44): 임시, 단일 항목 필터링 테스트용 코드. 추후에 대상 bitmap
  // 계산로직 작성 필요
  uint32_t using_idx = stoul(qc);
  const Roaring& r = reader_->bitmap_index_[using_idx];
  // WIP - 쿼리 대상 비트맵 포인터를 private member로 밀어넣기, Seek
  // / next시 roaring 이터레이터 위치 조정하도록 해야함. 포인터 소유권은
  // SABIIterator가 가지도록 구현해야함

  // 3. Block prefetch
  // TODO(TASK-46): Block prefetch, 완성된 쿼리 비트맵으로 탐색필요한 data
  // block prefetch 해올 수 있을지?
};

Status SABIIterator::SeekAndGetResult(const Slice& target,
                                      IterateResult* result) {
  // TODO: implement this
  // WIP - 기존 블록 인덱스 어떻게 쓸 수 있을지?
  // 1. 반환하는 행의 SST-local idx 도 반환필요
  // 2. (optional) target 이상의 key를 가지면서도 비트맵 필터에 의해
  // 방문해야하는 인덱스 엔트리중 가장 작은 인덱스 엔트리 방문 필요
  return Status::OK();
};

Status SABIIterator::NextAndGetResult(IterateResult* result) {
  // TODO: implement this
  cout << "NextAndGetResult called\n";
  return Status::OK();
};

UserDefinedIndexBuilder::BlockHandle SABIIterator::value() {
  // TODO: implement this
  // TODO: 테스트코드에서 훔쳐옴.. 로직 확인필요
  // UserDefinedIndexBuilder::BlockHandle handle{0, 0};
  // handle.offset = iter_->second.first.offset;
  // handle.size = iter_->second.first.size;
  // return handle;
};

// ========================================================================
// SABIReader Implementation
// ========================================================================

SABIReader::SABIReader(Slice& index_block) {
  // 1. Read footer
  uint32_t index_entries_cnt = DecodeFixed32(
      index_block.data() + index_block.size() - 2 * sizeof(uint32_t));
  uint32_t bitmap_cnt = DecodeFixed32(index_block.data() + index_block.size() -
                                      1 * sizeof(uint32_t));
  bitmap_index_.resize(bitmap_cnt);
  block_indices.resize(index_entries_cnt);

  // 2. Read Roaring offset vector
  uint32_t offset_cnt = bitmap_cnt + 1;
  vector<uint32_t> offsets(offset_cnt);
  const char* offset_table_offset = index_block.data() + index_block.size() -
                                    (2 + offset_cnt) * sizeof(uint32_t);
  for (uint32_t i = 0; i < offset_cnt; ++i) {
    offsets[i] = DecodeFixed32(offset_table_offset + i * sizeof(uint32_t));
  }

  // 3. Read Roaring
  for (uint32_t i = 0; i < bitmap_cnt; ++i) {
    uint32_t offset = offsets[i], size = offsets[i + 1] - offset;
    const char* raw_ptr = index_block.data() + offset;
    void* aligned_ptr = nullptr;
    // 32 bytes 정렬
    posix_memalign(&aligned_ptr, 32, size);
    AlignedPtr managed_aligned_ptr(static_cast<char*>(aligned_ptr), std::free);
    memcpy(managed_aligned_ptr.get(), raw_ptr, size);
    bitmap_index_[i] = Roaring::frozenView(
        reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
    managed_buffers_.push_back(std::move(managed_aligned_ptr)); // 소유권 이전
  }

  // 4. Read block index
  for (uint32_t i = 0; i < index_entries_cnt; ++i) {
    SABIBlockIndexEntry& entry = block_indices[i];
    Slice key;
    GetLengthPrefixedSlice(&index_block, &key);
    GetFixed64(&index_block, &entry.block_handle.offset);
    GetFixed64(&index_block, &entry.block_handle.size);
    GetFixed32(&index_block, &entry.prefix_kv_cnt);
    entry.index_key = key.ToString();
  }
}

unique_ptr<UserDefinedIndexIterator>
SABIReader::NewIterator(const ReadOptions& read_options) {
  return make_unique<SABIIterator>(this);
};

// The memory usage of the index, including the size of the raw contents and
// any other heap data structures allocated by the reader
size_t SABIReader::ApproximateMemoryUsage() const {
  // TODO: implement this
  return 0;
};

// ========================================================================
// SABIFactory Implementation
// ========================================================================

const char* SABIFactory::Name() const { return "SABIFactory"; }

UserDefinedIndexBuilder* SABIFactory::NewBuilder() const {
  return new SABIBuilder(options_);
}

unique_ptr<UserDefinedIndexReader>
SABIFactory::NewReader(Slice& index_block_) const {
  return unique_ptr<SABIReader>(new SABIReader(index_block_));
}

} // namespace bitmap_index
