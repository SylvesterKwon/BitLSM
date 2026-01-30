#include "roaring.hh"
#include "rocksdb/db.h"
#include <iomanip>
#include <iostream>

using namespace std;
using namespace rocksdb;
using namespace roaring;

// TODO: Implement index for effective block random access
class TablePropertiesSABIBuilder : public TablePropertiesCollector {
private:
  uint32_t data_entries_cnt_ = 0, cur_block_kv_cnt_ = 0;
  vector<uint32_t> block_kv_cnt_list_;
  vector<Roaring> roaring_set;

public:
  Status AddUserKey(const Slice& key, const Slice& value, EntryType type,
                    SequenceNumber seq, uint64_t file_size) override {
    // 현재 value encoding 예시:
    // 0000001000000000_d2D2Hxv7cXGvpUJo0TYy4tmIeDPw86Ye

    // future plan:
    // - 더 효율적인 value encoding 사용 (raw bit representation 보다 더
    // 압축적인)
    // - adaptive binning 구현

    string_view value_data = string_view(value.data());

    // 1. 자른 비트셋의 길이가 기존 roaring_set부터 크면 그만큼 사이즈 맞춤
    size_t underscore_pos = value_data.find('_');
    assert(underscore_pos != string::npos);
    if (roaring_set.size() < underscore_pos)
      roaring_set.resize(underscore_pos);

    // 2. 켜진 bit props는 bitset에 추가
    for (size_t i = 0; i < underscore_pos; ++i) {
      if (value_data[i] == '1') {
        roaring_set[i].add(data_entries_cnt_);
      }
    }

    ++cur_block_kv_cnt_, ++data_entries_cnt_;
    return Status::OK();
  }

  // Called after each new block is cut
  void BlockAdd(uint64_t block_uncomp_bytes,
                uint64_t block_compressed_bytes_fast,
                uint64_t block_compressed_bytes_slow) override {
    block_kv_cnt_list_.push_back(cur_block_kv_cnt_);
    cur_block_kv_cnt_ = 0;
  }

  Status Finish(UserCollectedProperties* properties) override {
    for (size_t i = 0; i < roaring_set.size(); ++i) {
      Roaring& r = roaring_set[i];
      r.runOptimize();
      string frozen_r;
      frozen_r.resize(r.getFrozenSizeInBytes());
      r.writeFrozen(frozen_r.data());

      // 읽을때 mmap으로 읽어야함
      // i-th bitmap: "bitmap_<i>"
      stringstream bitmap_idx_str;
      bitmap_idx_str << std::setw(4) << std::setfill('0') << i;
      std::string s = bitmap_idx_str.str();
      properties->insert({"bitmap_" + s, frozen_r});
    }

    // Insert serialized block_kv_cnt_list_
    uint32_t kv_cnt_sum = 0;
    for (uint32_t& cur_block_kv_cnt : block_kv_cnt_list_) {
      kv_cnt_sum += cur_block_kv_cnt;
      cur_block_kv_cnt = kv_cnt_sum;
    }
    string serialized_block_kv_cnt_psum_;
    serialized_block_kv_cnt_psum_.resize(block_kv_cnt_list_.size() *
                                         sizeof(uint32_t));
    memcpy(serialized_block_kv_cnt_psum_.data(), block_kv_cnt_list_.data(),
           block_kv_cnt_list_.size() * sizeof(uint32_t));
    properties->insert({"block_kv_cnt_psum", serialized_block_kv_cnt_psum_});

    return Status::OK();
  }

  const char* Name() const override { return "TablePropertiesSABIBuilder"; }

  UserCollectedProperties GetReadableProperties() const override {
    return UserCollectedProperties{}; // TODO: 디버그용 맵 구현하기
  }
};

class TablePropertiesSABIBuilderFactory
    : public TablePropertiesCollectorFactory {
public:
  TablePropertiesCollector* CreateTablePropertiesCollector(
      TablePropertiesCollectorFactory::Context context) override {
    // 필요시 context를 TablePropertiesSABIBuilder class 로 전달하기 (level 등)
    return new TablePropertiesSABIBuilder();
  }

  const char* Name() const override {
    return "TablePropertiesSABIBuilderFactory";
  }
};
