#include <iostream>

#include "rocksdb/options.h"
#include "table/block_based/block_based_table_reader.h"

using namespace std;
using namespace rocksdb;

namespace sabi {

class BlockBasedTableSABIFilter {
public:
  BlockBasedTableSABIFilter(BlockBasedTable* bbt) { this->bbt = bbt; }
  void test() {
    // 모든 블록 핸들 가져오는 예제,
    IndexBlockIter input_iter;

    // TODO: get_context, lookup_context가 뭔지 확인 필요
    InternalIteratorBase<IndexValue>* iiter = bbt->NewIndexIterator(
        ReadOptions(), false, &input_iter, nullptr, nullptr);

    for (iiter->SeekToFirst(); iiter->Valid(); iiter->Next()) {
      const BlockHandle& bh = iiter->value().handle;
      // TODO: 여기서 뭘 할 수 있을까? sparse table?
    }
  }

  void get_all_block_filter() {
    // test only. 실제로 인터페이스는 어떻게 생겨야될지 고민 해보기

    // cout << "debug: " <<  << "\n";

    // unique_ptr<TableReader> table_reader;
    // table_reader

    // FindTable ?
  }

private:
  BlockBasedTable* bbt;
};

} // namespace sabi
