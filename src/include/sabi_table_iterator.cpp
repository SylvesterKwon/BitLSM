#include "table/block_based/block_based_table_reader.h"
#include <iostream>

using namespace std;
using namespace rocksdb;

// Table Iterator for SST with SABI
// WIP - 자체 iterator 구상중
class SABITableIterator {
private:
  BlockBasedTable* bbt;
  uint32_t block_restart_interval;

public:
  SABITableIterator(BlockBasedTable* bbt) {
    this->bbt = bbt;

    block_restart_interval =
        bbt->get_rep()->table_options.block_restart_interval;
  }

  void test() {
    BlockBasedTable::IndexReader* index_reader =
        bbt->get_rep()->index_reader.get();
    IndexBlockIter input_iter; // TODO: 역할 뭔지 확인 필요
    InternalIteratorBase<IndexValue>* iiter = index_reader->NewIterator(
        ReadOptions(), false, &input_iter, nullptr, nullptr);
    iiter->SeekToFirst();
    const BlockHandle& bh = iiter->value().handle;
    // 이후 얻은 bh로 get_all_by_indexes_from_data_block
  }

  // Get all data entries by indexes from data block
  // indexes must be sorted and unique
  void get_all_by_indexes_from_data_block(const BlockHandle& bh,
                                          vector<uint32_t>& indexes,
                                          vector<PinnableSlice>& out_keys,
                                          vector<PinnableSlice>& out_values) {
    DataBlockIter biter;
    Status s;
    out_keys.resize(indexes.size());
    out_values.resize(indexes.size());
    // TODO: nullptr 로 미사용중인 옵션을 통해 최적화 가능 여부 확인하기
    bbt->NewDataBlockIterator(ReadOptions(), bh, &biter, BlockType::kData,
                              nullptr, nullptr, nullptr, false, false, s, true);

    uint32_t cur_checkpoint =
        UINT32_MAX; // UINT32_MAX means no valid checkpoint is used
    uint32_t cur_offset = 0;
    uint32_t result_idx = 0;

    for (uint32_t i = 0; i < indexes.size(); i++) {
      uint32_t target_checkpoint = indexes[i] / block_restart_interval;
      if (cur_checkpoint != target_checkpoint) {
        cur_checkpoint = target_checkpoint;
        cur_offset = 0;
        biter.SeekToRestartPoint(cur_checkpoint);
        biter.Next(); // need to call Next once to access real data
      }
      uint32_t target_offset = indexes[i] % block_restart_interval;
      while (cur_offset < target_offset && biter.Valid()) {
        biter.Next();
        cur_offset++;
      }
      if (biter.Valid()) {
        // PinSelf vs PinSlice (zero-copy)
        // TODO(TASK-93): Block cache 사용할 수 있도록 최적화 하기.
        // PinSelf 방식은 hard-copy임
        out_keys[i].PinSelf(biter.key());
        out_values[i].PinSelf(biter.value());
      } else {
        assert(false);
      }
    }
  }

  void example_get_all_by_indexes_from_data_block() {
    // 1. 요청할 인덱스
    std::vector<uint32_t> my_indexes = {10, 5, 100};

    // 2. 결과를 담을 배열 미리 생성
    // 중요, 크기 미리 잡아둬야함!!!
    size_t num_items = my_indexes.size();
    std::vector<PinnableSlice> keys(num_items);
    std::vector<PinnableSlice> values(num_items);

    // 3. 함수 호출 (vector의 내부 데이터 포인터를 넘김)
    // &keys[0] 또는 keys.data()를 사용하면 PinnableSlice* 타입이 됨
    // get_all_by_indexes_from_data_block(
    //     handle, my_indexes,
    //     keys.data(),  // PinnableSlice* (시작 주소)
    //     values.data() // PinnableSlice* (시작 주소)
    // );
  }
};
