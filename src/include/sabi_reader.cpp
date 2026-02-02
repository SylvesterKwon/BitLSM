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

// TODO: Block random iteration 로직 가져와서 통합하기
namespace bitmap_index {

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
  const Roaring& r = reader_->bitmap_index_.bitmaps[using_idx];
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

SABIReader::SABIReader(Slice& index_block, SABIOptions options)
    : options_(options) {
  // 1. Read footer
  uint32_t index_entries_cnt_ = DecodeFixed32(
      index_block.data() + index_block.size() - 3 * sizeof(uint32_t));
  uint32_t bitmap_index_offset_offset = DecodeFixed32(
      index_block.data() + index_block.size() - 2 * sizeof(uint32_t));
  uint32_t binning_policy_offset_offset = DecodeFixed32(
      index_block.data() + index_block.size() - 1 * sizeof(uint32_t));

  data_entries_cnt_psum_.resize(index_entries_cnt_);

  // 2. Read binning policy
  uint32_t binning_policy_offset_cnt =
      DecodeFixed32(index_block.data() + binning_policy_offset_offset);
  uint32_t binning_policy_cnt = binning_policy_offset_cnt - 1;
  bitmap_index_.binning_policy.resize(binning_policy_cnt);
  bitmap_index_.bitmap_nums.resize(binning_policy_cnt);
  assert(options_.sk_num == binning_policy_cnt);

  for (uint32_t i = 0; i < binning_policy_cnt; ++i) {
    uint32_t cur_binning_policy_offset =
        DecodeFixed32(index_block.data() + binning_policy_offset_offset +
                      (i + 1) * sizeof(uint32_t));
    uint32_t cur_binning_policy_size =
        DecodeFixed32(index_block.data() + cur_binning_policy_offset);
    bitmap_index_.bitmap_nums[i] = cur_binning_policy_size;

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
      bitmap_index_.binning_policy[i] = std::move(cur_binning_policy);
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      vector<double> cur_binning_policy(cur_binning_policy_size);
      for (uint32_t j = 0; j < cur_binning_policy_size; ++j) {
        uint64_t val_int =
            DecodeFixed64(index_block.data() + cur_binning_policy_offset +
                          sizeof(uint32_t) + j * sizeof(double));
        memcpy(&cur_binning_policy[j], &val_int, sizeof(double));
      }
      bitmap_index_.binning_policy[i] = std::move(cur_binning_policy);
    } else {
      assert(false);
    }
  }

  // 3. Read bitmap index offset
  uint32_t bitmap_offset_cnt =
      DecodeFixed32(index_block.data() + bitmap_index_offset_offset);
  uint32_t bitmaps_cnt = bitmap_offset_cnt - 1;
  bitmap_index_.bitmaps.resize(bitmaps_cnt);
  vector<uint32_t> bitmap_offsets(bitmap_offset_cnt);
  for (uint32_t i = 0; i < bitmap_offset_cnt; ++i) {
    bitmap_offsets[i] =
        DecodeFixed32(index_block.data() + bitmap_index_offset_offset +
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
    bitmap_index_.bitmaps[i] = Roaring::frozenView(
        reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
    managed_buffers_.push_back(
        std::move(managed_aligned_ptr)); // move pointer ownership
  }

  // 4. Read index block prefix sum
  for (uint32_t i = 0; i < index_entries_cnt_; ++i) {
    data_entries_cnt_psum_[i] =
        DecodeFixed32(index_block.data() + i * sizeof(uint32_t));
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

} // namespace bitmap_index
