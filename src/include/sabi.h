#include "roaring.hh"
#include "rocksdb/user_defined_index.h"
#include <cstddef>
#include <iostream>
#include <memory>

using namespace std;
using namespace rocksdb;
using namespace roaring;

// TODO: Block random iteration 로직 가져와서 통합하기
namespace bitmap_index {

struct QueryCondition;
class SABIBuilder;
class SABIIterator;
class SABIReader;
class SABIFactory;

class SABIBuilder : public UserDefinedIndexBuilder {
private:
  uint32_t cur_table_kv_cnt_ = 0, cur_block_kv_cnt_ = 0;
  vector<Roaring> roaring_set_;
  string final_index_blob_;

public:
  Slice AddIndexEntry(const Slice& last_key_in_current_block,
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

  void OnKeyAdded(const Slice& key, ValueType type, const Slice& value) {
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

  Status Finish(Slice* index_contents) {
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
};

class SABIIterator : public UserDefinedIndexIterator {
private:
  const SABIReader* reader_;
  const ScanOptions* scan_opts_;
  size_t num_opts_;

public:
  SABIIterator(const SABIReader* reader) : reader_(reader) {
    cout << "SABIIterator instantiated\n";
  }

  void Prepare(const ScanOptions scan_opts[], size_t num_opts) {
    // TODO: implement this
    scan_opts_ = scan_opts;
    num_opts_ = num_opts;
    // query condition대로 방문할 블록 핸들 벡터로 private member로 저장해야함.
    cout << "prepare called! num_opts: " << num_opts << "\n";
  };

  Status SeekAndGetResult(const Slice& target, IterateResult* result) {
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

  Status NextAndGetResult(IterateResult* result) {
    // TODO: implement this
    cout << "NextAndGetResult called\n";
    return Status::OK();
  };

  UserDefinedIndexBuilder::BlockHandle value() {
    // TODO: implement this
  };
};

class SABIReader : public UserDefinedIndexReader {
private:
  Slice index_block;
  vector<uint32_t> offsets;
  vector<Roaring> roaring_set_;
  using AlignedPtr = unique_ptr<char[], void (*)(void*)>;
  // posix_memalign으로 할당받은 메모리 누수를 막기 위해
  vector<AlignedPtr> managed_buffers_;

public:
  SABIReader(Slice& index_block) : index_block(index_block) {
    // 1. Footer 읽기
    assert(index_block.size() >= sizeof(uint32_t));
    uint32_t bitmap_count;
    memcpy(&bitmap_count,
           index_block.data() + index_block.size() - sizeof(uint32_t),
           sizeof(uint32_t));
    roaring_set_.resize(bitmap_count);

    // 2. offset list 읽기
    offsets.resize(bitmap_count + 1);
    memcpy(offsets.data(),
           index_block.data() + index_block.size() - sizeof(uint32_t) -
               offsets.size() * sizeof(uint32_t),
           offsets.size() * sizeof(uint32_t));

    // 3. Roaring 읽기
    for (uint32_t i = 0; i < bitmap_count; ++i) {
      uint32_t offset = offsets[i], size = offsets[i + 1] - offset;
      const char* raw_ptr = index_block.data() + offset;
      void* aligned_ptr = nullptr;
      // 32 bytes 정렬
      assert(posix_memalign(&aligned_ptr, 32, size) == 0);
      AlignedPtr managed_aligned_ptr(static_cast<char*>(aligned_ptr),
                                     std::free);
      memcpy(managed_aligned_ptr.get(), raw_ptr, size);
      roaring_set_[i] = Roaring::frozenView(
          reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
      managed_buffers_.push_back(std::move(managed_aligned_ptr)); // 소유권 이전
    }
  }

  unique_ptr<UserDefinedIndexIterator>
  NewIterator(const ReadOptions& read_options) {
    return make_unique<SABIIterator>(this);
  };

  // The memory usage of the index, including the size of the raw contents and
  // any other heap data structures allocated by the reader
  size_t ApproximateMemoryUsage() const {
    // TODO: implement this
    return 0;
  };
};

class SABIFactory : public UserDefinedIndexFactory {
public:
  const char* Name() const override { return "SABIFactory"; }

  UserDefinedIndexBuilder* NewBuilder() const override {
    return new SABIBuilder();
  }

  unique_ptr<UserDefinedIndexReader>
  NewReader(Slice& index_block) const override {
    return unique_ptr<SABIReader>(new SABIReader(index_block));
  }
};

} // namespace bitmap_index
