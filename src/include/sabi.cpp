#include <iostream>
#include <sabi.h>

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
  // TODO: Block handle 을 직접 가져올 수 있음. 이 함수 이용해서 block random
  // access 구현 가능
  // implement here...
  cur_block_kv_cnt_ = 0;

  // 기존 Index Builder에서는 반환값을 key separator로 사용하지만,
  // 현재 UserDefinedIndexBuilderWrapper 구현에서는 버려짐.
  return last_key_in_current_block;
}

void SABIBuilder::OnKeyAdded(const Slice& key, ValueType type,
                             const Slice& value) {
  string_view value_data = string_view(value.data());

  // 1. 자른 비트셋의 길이가 기존 roaring_set_부터 크면 그만큼 사이즈 맞춤
  size_t underscore_pos = value_data.find('_');
  assert(underscore_pos != string::npos);
  if (roaring_set_.size() < underscore_pos)
    roaring_set_.resize(underscore_pos);

  // 2. 켜진 bit props는 bitset에 추가
  for (size_t i = 0; i < underscore_pos; ++i) {
    if (value_data[i] == '1') {
      roaring_set_[i].add(cur_table_kv_cnt_);
    }
  }

  ++cur_block_kv_cnt_, ++cur_table_kv_cnt_;
}

Status SABIBuilder::Finish(Slice* index_contents) {
  vector<uint32_t> offsets;
  uint32_t total_roaring_size = 0,
           total_offset_table_size = 0; // size in bytes

  offsets.push_back(0);
  for (uint32_t i = 0; i < roaring_set_.size(); ++i) {
    Roaring& r = roaring_set_[i];
    r.runOptimize();
    total_roaring_size += r.getFrozenSizeInBytes();
    offsets.push_back(offsets.back() + r.getFrozenSizeInBytes());
  }

  total_offset_table_size = offsets.size() * sizeof(uint32_t);

  // total index size = total_roaring_size + total_offset_table_size + footer
  final_index_blob_.resize(total_roaring_size + total_offset_table_size +
                           1 * sizeof(uint32_t));

  for (uint32_t i = 0; i < roaring_set_.size(); ++i) {
    Roaring& r = roaring_set_[i];
    r.writeFrozen(final_index_blob_.data() + offsets[i]);
  }

  memcpy(final_index_blob_.data() + total_roaring_size, offsets.data(),
         offsets.size() * sizeof(uint32_t));
  uint32_t bitmap_count = roaring_set_.size();
  memcpy(final_index_blob_.data() + total_roaring_size +
             total_offset_table_size,
         &bitmap_count, sizeof(uint32_t));

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

  // 2. bitset filtering 사용하여 방문필요한 key 정리
  // TODO: 이후에 복합 쿼리 들어오면 이곳에서 필요한 모든 전처리 (bitset
  // 조합등) 진행할 것
  // WIP - implementing

  /*
  메모...
  - 사실 거의 다 방문해야하는것 아닌가?...
  */

  // 3. ?

  // TODO - query condition대로 방문할 블록 핸들 벡터로 private member로
  // 저장해야함.
};

Status SABIIterator::SeekAndGetResult(const Slice& target,
                                      IterateResult* result) {
  // TODO: implement this
  /*

  WIP - 쿼리 조건을 어떻게 받아야와햐는지? target써도 될려나?

  구현 계획:
  1. Prepare()에서 일단 이 SST-local 한 쿼리결과 계산
    1.1 쿼리용 비트맵 계산
    1.2 검색 후보 블록 계산
  2. SeekAndGetResult() 구현
  3. NextAndGetResult() 구현
  */
  ///////////////////////////////
  cout << "target: " << target.data() << "\n";
  return Status::OK();
};

Status SABIIterator::NextAndGetResult(IterateResult* result) {
  // TODO: implement this
  cout << "NextAndGetResult called\n";
  return Status::OK();
};

UserDefinedIndexBuilder::BlockHandle SABIIterator::value() {
  // TODO: implement this
};

// ========================================================================
// SABIReader Implementation
// ========================================================================

SABIReader::SABIReader(Slice& index_block) : index_block_(index_block) {
  // 1. Footer 읽기
  assert(index_block_.size() >= sizeof(uint32_t));
  uint32_t bitmap_count;
  memcpy(&bitmap_count,
         index_block_.data() + index_block_.size() - sizeof(uint32_t),
         sizeof(uint32_t));
  roaring_set_.resize(bitmap_count);

  // 2. offset list 읽기
  offsets.resize(bitmap_count + 1);
  memcpy(offsets.data(),
         index_block_.data() + index_block_.size() - sizeof(uint32_t) -
             offsets.size() * sizeof(uint32_t),
         offsets.size() * sizeof(uint32_t));

  // 3. Roaring 읽기
  for (uint32_t i = 0; i < bitmap_count; ++i) {
    uint32_t offset = offsets[i], size = offsets[i + 1] - offset;
    const char* raw_ptr = index_block_.data() + offset;
    void* aligned_ptr = nullptr;
    // 32 bytes 정렬
    assert(posix_memalign(&aligned_ptr, 32, size) == 0);
    AlignedPtr managed_aligned_ptr(static_cast<char*>(aligned_ptr), std::free);
    memcpy(managed_aligned_ptr.get(), raw_ptr, size);
    roaring_set_[i] = Roaring::frozenView(
        reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
    managed_buffers_.push_back(std::move(managed_aligned_ptr)); // 소유권 이전
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
  return new SABIBuilder();
}

unique_ptr<UserDefinedIndexReader>
SABIFactory::NewReader(Slice& index_block_) const {
  return unique_ptr<SABIReader>(new SABIReader(index_block_));
}

} // namespace bitmap_index
