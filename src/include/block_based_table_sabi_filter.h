#include <iostream>

#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_fetcher.h"
#include "table/format.h"

using namespace std;
using namespace rocksdb;

namespace sabi {

class BlockBasedTableSABIFilter {
public:
  BlockBasedTableSABIFilter(BlockBasedTable* bbt) { this->bbt = bbt; }

  // SST의 index block을 전체 iteration도는 예시
  void test_index_block_full_iteration() {
    BlockBasedTable::IndexReader* index_reader =
        bbt->get_rep()->index_reader.get();

    IndexBlockIter input_iter; // TODO: 역할 뭔지 확인 필요
    InternalIteratorBase<IndexValue>* iiter = index_reader->NewIterator(
        ReadOptions(), false, &input_iter, nullptr, nullptr);

    for (iiter->SeekToFirst(); iiter->Valid(); iiter->Next()) {
      const BlockHandle& bh = iiter->value().handle;
      cout << bh.offset() << " " << bh.size() << "\n";
      // TODO: 여기서 뭘 할 수 있을까? sparse table?
    }
  }

  // 테스트 block 순회
  void test_block_iteration(const BlockHandle& bh) {

    // BlockBasedTable::NewDataBlockIterator<DataBlockIter>
    Status s;
    DataBlockIter biter;
    bbt->NewDataBlockIterator(ReadOptions(), bh, &biter, BlockType::kData,
                              nullptr, nullptr, nullptr, false, false, s, true);

    uint32_t block_restart_interval =
        bbt->get_rep()->table_options.block_restart_interval;

    // TODO: vector<uint32_t> 에 해당하는 애들만 빼올 수 있어야함.
    // 이터레이터스럽게 구현필요

    for (uint32_t i = 0; i < biter.get_num_restarts_(); i++) {
      biter.SeekToRestartPoint(i);
      biter.Next(); // checkpoint block에서 실제 data 접근을 위해서 한번 더 이동
      cout << "checkpoint " << i << " ----------\n";
      for (uint32_t j = 0; j < block_restart_interval && biter.Valid();
           j++, biter.Next()) {
        cout << biter.key().ToString() << " " << biter.value().ToString()
             << "\n";
      }
    }
  }

  void test() {
    BlockBasedTable::IndexReader* index_reader =
        bbt->get_rep()->index_reader.get();
    IndexBlockIter input_iter; // TODO: 역할 뭔지 확인 필요
    InternalIteratorBase<IndexValue>* iiter = index_reader->NewIterator(
        ReadOptions(), false, &input_iter, nullptr, nullptr);
    iiter->SeekToFirst();
    const BlockHandle& bh = iiter->value().handle;
    test_block_iteration(bh);
  }

private:
  BlockBasedTable* bbt;
};

} // namespace sabi
