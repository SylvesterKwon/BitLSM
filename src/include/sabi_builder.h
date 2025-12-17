#include <iostream>

#include "roaring.hh"
#include "rocksdb/db.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

// TODO: Implement index for effective block random access
class SABIBuilder : public TablePropertiesCollector {
private:
  uint32_t cur_block_kv_cnt_ = 0;
  vector<uint32_t> block_kv_cnt_list_;
  vector<bool> bits_; // 비트들을 임시로 모아둘 벡터
                      // TODO: vector<bool> 보다 더 빠른 표현 찾기.

public:
  Status AddUserKey(const Slice& key, const Slice& value, EntryType type,
                    SequenceNumber seq, uint64_t file_size) override {
    // WIP - 비트셋 여기서 빌드하면 됨
    cur_block_kv_cnt_++;
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
    // TODO: insert bitmap
    // foreach...
    // properties->insert({"bitmap", ToString(max_ts_)});

    // Insert serialized block_kv_cnt_list_
    uint32_t kv_cnt_sum = 0;
    for (uint32_t& cur_block_kv_cnt : block_kv_cnt_list_) {
      kv_cnt_sum += cur_block_kv_cnt;
      cur_block_kv_cnt = kv_cnt_sum;
    }
    string serialized_block_kv_cnt_psum_;
    serialized_block_kv_cnt_psum_.resize(block_kv_cnt_list_.size() *
                                         sizeof(uint32_t));
    memcpy(&serialized_block_kv_cnt_psum_[0], block_kv_cnt_list_.data(),
           block_kv_cnt_list_.size() * sizeof(uint32_t));
    properties->insert({"block_kv_cnt_psum", serialized_block_kv_cnt_psum_});

    return Status::OK();
  }

  const char* Name() const override { return "SABIBuilder"; }

  UserCollectedProperties GetReadableProperties() const override {
    return UserCollectedProperties{}; // TODO: 디버그용 맵 구현하기
  }
};

class SABIBuilderFactory : public TablePropertiesCollectorFactory {
public:
  TablePropertiesCollector* CreateTablePropertiesCollector(
      TablePropertiesCollectorFactory::Context context) override {
    return new SABIBuilder();
  }

  const char* Name() const override { return "SABIBuilderFactory"; }
};
