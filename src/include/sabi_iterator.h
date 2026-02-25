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
  SABIOptions options_;
  rocksdb::BlockBasedTable* bbt_;
  rocksdb::BlockBasedTable::IndexReader* index_reader_;
  SABIReader* sabi_reader_;
  uint32_t block_restart_interval;
  std::vector<rocksdb::BlockHandle> target_bhs_; // target block handles

  // Get all data entries by indexes from data block
  // indexes must be sorted and unique
  void get_all_by_indexes_from_data_block(
      const rocksdb::BlockHandle& bh, std::vector<uint32_t>& indexes,
      std::vector<rocksdb::PinnableSlice>& out_keys,
      std::vector<rocksdb::PinnableSlice>& out_values);

  // Get bitmap for given query condition
  roaring::Roaring GetBitmapFromQuery(const SABIQuery& query);

public:
  SABITableIterator(SABIOptions options, rocksdb::BlockBasedTable* bbt,
                    SABIQuery query);
  void test();
};

} // namespace bitmap_index
