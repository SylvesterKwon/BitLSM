#include "roaring.hh"
#include "rocksdb/user_defined_index.h"
#include <iostream>

using namespace std;
using namespace rocksdb;
using namespace roaring;

class SABIBuilder : public UserDefinedIndexBuilder {
private:
  uint32_t cur_table_kv_cnt_ = 0, cur_block_kv_cnt_ = 0;
  vector<Roaring> roaring_set;
  string final_index_blob;

public:
  Slice AddIndexEntry(const Slice& last_key_in_current_block,
                      const Slice* first_key_in_next_block,
                      const BlockHandle& block_handle,
                      string* separator_scratch) {

    // cout << "[AddIndexEntry] " << block_handle.offset << " "
    //      << block_handle.size << "\n";

    // TODO: 얘 아무 동작도 안하는것 같은데. 확인 중
  }

  void OnKeyAdded(const Slice& key, ValueType type, const Slice& value) {
    string_view value_data = string_view(value.data());

    // 1. 자른 비트셋의 길이가 기존 roaring_set부터 크면 그만큼 사이즈 맞춤
    size_t underscore_pos = value_data.find('_');
    assert(underscore_pos != string::npos);
    if (roaring_set.size() < underscore_pos)
      roaring_set.resize(underscore_pos);

    // 2. 켜진 bit props는 bitset에 추가
    for (size_t i = 0; i < underscore_pos; ++i) {
      if (value_data[i] == '1') {
        roaring_set[i].add(cur_table_kv_cnt_);
      }
    }

    ++cur_block_kv_cnt_, ++cur_table_kv_cnt_;
  }

  Status Finish(Slice* index_contents) {
    vector<uint32_t> offsets;
    // size in bytes
    uint32_t total_roaring_size = 0, total_offset_table_size = 0;

    offsets.push_back(0);
    for (uint32_t i = 0; i < roaring_set.size(); ++i) {
      Roaring& r = roaring_set[i];
      r.runOptimize();
      total_roaring_size += r.getFrozenSizeInBytes();
      offsets.push_back(offsets.back() + r.getFrozenSizeInBytes());
    }

    total_offset_table_size = offsets.size() * sizeof(uint32_t);

    // total index size = total_roaring_size + total_offset_table_size + footer
    final_index_blob.resize(total_roaring_size + total_offset_table_size +
                            1 * sizeof(uint32_t));

    for (uint32_t i = 0; i < roaring_set.size(); ++i) {
      Roaring& r = roaring_set[i];
      r.writeFrozen(final_index_blob.data() + offsets[i]);
    }

    memcpy(final_index_blob.data() + total_roaring_size, offsets.data(),
           offsets.size() * sizeof(uint32_t));
    uint32_t bitmap_count = roaring_set.size();
    memcpy(final_index_blob.data() + total_roaring_size +
               total_offset_table_size,
           &bitmap_count, sizeof(uint32_t));

    *index_contents = Slice(final_index_blob);
    return Status::OK();
  }
};

class SABIIterator : public UserDefinedIndexIterator {
public:
  void Prepare(const ScanOptions scan_opts[], size_t num_opts) {
    // TODO: implement this
  };

  Status SeekAndGetResult(const Slice& target, IterateResult* result) {
    // TODO: implement this
  };

  // Advance to the next index entry. The result must be populated similar
  // to SeekAndGetResult.
  Status NextAndGetResult(IterateResult* result) {
    // TODO: implement this
  };

  // Return the BlockHandle in the current index entry
  UserDefinedIndexBuilder::BlockHandle value() {
    // TODO: implement this
  };
};

class SABIReader : public UserDefinedIndexReader {
private:
  Slice index_block;
  vector<uint32_t> offsets;
  vector<Roaring> roaring_set;
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
    roaring_set.resize(bitmap_count);

    // 2. offset list 읽기
    offsets.resize(bitmap_count + 1);
    memcpy(offsets.data(),
           index_block.data() + index_block.size() - sizeof(uint32_t) -
               offsets.size() * sizeof(uint32_t),
           offsets.size() * sizeof(uint32_t));
    for (auto& oi : offsets)
      cout << oi << " ";
    cout << "\n";

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
      roaring_set[i] = Roaring::frozenView(
          reinterpret_cast<const char*>(managed_aligned_ptr.get()), size);
      managed_buffers_.push_back(std::move(managed_aligned_ptr)); // 소유권 이전

      // for test
      cout << "cardinality " << i << "-" << roaring_set[i].cardinality()
           << " / " << roaring_set[i].maximum() << "\n";
    }
  }

  unique_ptr<UserDefinedIndexIterator>
  NewIterator(const ReadOptions& read_options) {

    return unique_ptr<SABIIterator>();
    // TODO: implement this
  };

  // The memory usage of the index, including the size of the raw contents and
  // any other heap data structures allocated by the reader
  size_t ApproximateMemoryUsage() const {
    // TODO: implement this
    return 0;
  };
};

class SABIBuilderFactory : public UserDefinedIndexFactory {
public:
  const char* Name() const override { return "SABIBuilderFactory"; }

  UserDefinedIndexBuilder* NewBuilder() const override {
    return new SABIBuilder();
  }

  unique_ptr<UserDefinedIndexReader>
  NewReader(Slice& index_block) const override {
    return unique_ptr<SABIReader>(new SABIReader(index_block));
  }
};