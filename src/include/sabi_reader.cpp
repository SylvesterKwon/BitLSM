#include "util/coding.h"
#include "util/coding_lean.h"
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
  cout << "SABI READER INIT\n";
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

} // namespace bitmap_index
