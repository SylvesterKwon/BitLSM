#pragma once

#define TEST_CACHE_LINE_SIZE                                                   \
  64 // To avoid compile error when using roaring.hh &
     // block_based_table_reader.h together

#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include <sabi_query.h>

namespace bitmap_index {

// Table Iterator for SST with SABI
// WIP - 자체 iterator 구상중
class SABITableIterator {
private:
  // Table & query context
  SABIOptions options_;
  rocksdb::BlockBasedTable* bbt_;
  rocksdb::BlockBasedTable::IndexReader* index_reader_;
  SABIReader* sabi_reader_;
  uint32_t block_restart_interval_;

  // Internal status for iterating
  roaring::Roaring query_bitmap_;                // bitmap for current iteration
  roaring::Roaring::const_iterator bitmap_iter_; // bitmap iterator
  roaring::Roaring::const_iterator bitmap_end_;
  bool valid_ = false;
  std::vector<std::pair<uint32_t,
                        rocksdb::BlockHandle>>
      target_blocks_;                   // {index, blockhandle}
  int32_t cur_target_block_idx_ = -1;   // current index for target_blocks_
  std::vector<uint32_t> local_indexes_; // buffer for block-local index
  std::vector<rocksdb::PinnableSlice> keys_buffer_;
  std::vector<rocksdb::PinnableSlice> values_buffer_;
  int32_t buffer_idx_ = 0; // 버퍼 내 현재 커서

  // Get all data entries by indexes from data block
  // indexes must be sorted and unique
  void
  GetAllByIndexesFromDataBlock(const rocksdb::BlockHandle& bh,
                               std::vector<uint32_t>& indexes,
                               std::vector<rocksdb::PinnableSlice>& out_keys,
                               std::vector<rocksdb::PinnableSlice>& out_values);

  // Get bitmap for given query condition
  roaring::Roaring GetBitmapFromQuery(const SABIQuery& query);
  // Fill buffer by loading next data block
  void LoadNextBlock();

public:
  SABITableIterator(SABIOptions options, rocksdb::BlockBasedTable* bbt,
                    SABIQuery query);
  void Next(); // Get Next Data Entries
  bool Valid();
  // TODO: 값 조회 로직도 추가 필!
  void test();
};

} // namespace bitmap_index
