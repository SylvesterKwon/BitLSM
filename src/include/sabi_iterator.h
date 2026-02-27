#pragma once

#include "db/version_set.h"

#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include <sabi_query.h>

namespace bitmap_index {

// Abstract class for internal iterator SABITableIterator, SABIMemTableIterator
class SABIInternalIterator {
protected:
  bool valid_ = false;

public:
  virtual ~SABIInternalIterator() {}
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  bool Valid() const { return valid_; };
  virtual rocksdb::Slice key() const = 0;
  virtual rocksdb::Slice value() const = 0;
};

class SABIMergingIterator;
class SABILevelIterator;
class SABITableIterator;
// class SABIMemTableIterator;

// Merging Iterator for SABI. Internally contains SABITableIterator and
// MemTableIterator
class SABIMergingIterator {
private:
  // Table & query context
  const SABIOptions options_;
  const SABIQuery query_;
  const rocksdb::ColumnFamilyData* cfd_;
  rocksdb::Version* v_;
  rocksdb::TableCache* tc_;
  const rocksdb::VersionStorageInfo* storage_info_;
  const rocksdb::InternalKeyComparator* icmp_;
  const rocksdb::MutableCFOptions& cf_opts_;

  // Internal status for iterating
  // -

public:
  SABIMergingIterator(rocksdb::ColumnFamilyData* cfd, SABIOptions options,
                      SABIQuery query);
  ~SABIMergingIterator();
  void SeekToFirst();
  void Next(); // Get Next Data Entries
  // TODO: Merging iterator는 internal iterator 상속받아야하는가? (internal
  // 아니고 sabiiterator같은공통개념)
};

// Level Iterator for SST with SABI
class SABILevelIterator : public SABIInternalIterator {
private:
  // Table & query context
  uint32_t level_;
  const SABIOptions options_;
  const SABIQuery query_;
  const rocksdb::ColumnFamilyData* cfd_;
  rocksdb::Version* v_;
  rocksdb::TableCache* tc_;
  const rocksdb::VersionStorageInfo* storage_info_;
  const rocksdb::InternalKeyComparator* icmp_;
  const rocksdb::MutableCFOptions& cf_opts_;
  const std::vector<rocksdb::FileMetaData*>& files_;

  // Internal status for iterating
  uint32_t cur_file_idx_;
  rocksdb::TableCache::TypedHandle* cur_table_handle_;
  SABITableIterator* cur_sti_;

  void LoadFile(size_t idx); // Open file with index

public:
  SABILevelIterator(rocksdb::ColumnFamilyData* cfd, uint32_t level,
                    SABIOptions options, SABIQuery query);
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

// Table Iterator for SST with SABI
class SABITableIterator : public SABIInternalIterator {
private:
  // Table & query context
  SABIOptions options_;
  rocksdb::BlockBasedTable* bbt_;
  rocksdb::BlockBasedTable::IndexReader* index_reader_;
  SABIReader* sabi_reader_;
  SABIQuery query_;
  uint32_t block_restart_interval_;

  // Internal status for iterating
  roaring::Roaring query_bitmap_;                // bitmap for current iteration
  roaring::Roaring::const_iterator bitmap_iter_; // bitmap iterator
  roaring::Roaring::const_iterator bitmap_end_;
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
  bool CheckCondition(rocksdb::Slice value);

public:
  SABITableIterator(SABIOptions options, rocksdb::BlockBasedTable* bbt,
                    SABIQuery query);
  void SeekToFirst() override;
  void Next() override; // Get Next Data Entries
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

} // namespace bitmap_index
