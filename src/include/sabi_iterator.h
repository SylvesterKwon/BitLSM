#pragma once

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"

#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include <cstdint>
#include <queue>
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

class SABIIterator;
class SABIMergingIterator;
class SABILevelIterator;
class SABITableIterator;
class SABIMemTableIterator;

// Iterator for SABI
class SABIIterator : public SABIInternalIterator {
private:
  rocksdb::DB* db_;
  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyHandle* cfh_; // To use multiget API
  const rocksdb::Snapshot* snapshot_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::SuperVersion* sv_;

  SABIMergingIterator* smi_;
  SABIOptions options_;
  SABIQuery query_;

  // batch
  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;

  std::string latest_user_key_added;

  void FetchNextBatch(uint32_t batch_size);

public:
  SABIIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfh,
               SABIOptions options, SABIQuery query);
  ~SABIIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
};

// Iterator comparator for SABIMergingIterator
struct IteratorComparator {
  const rocksdb::InternalKeyComparator* icmp_;
  IteratorComparator(const rocksdb::InternalKeyComparator* icmp = nullptr)
      : icmp_(icmp) {}
  bool operator()(const SABIInternalIterator* a,
                  const SABIInternalIterator* b) const {
    return icmp_->Compare(a->key(), b->key()) > 0;
  }
};

// Merging Iterator for SABI. Internally contains SABITableIterator and
// MemTableIterator
class SABIMergingIterator : public SABIInternalIterator {
private:
  // Table & query context
  const SABIOptions options_;
  const SABIQuery query_;
  rocksdb::SuperVersion* sv_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::Version* v_;
  rocksdb::TableCache* tc_;
  const rocksdb::VersionStorageInfo* storage_info_;
  const rocksdb::InternalKeyComparator* icmp_;
  const rocksdb::MutableCFOptions& cf_opts_;
  std::vector<rocksdb::TableCache::TypedHandle*> l0_handles_; // L0 handles

  // Internal status for iterating
  std::vector<SABIInternalIterator*> ch_iters_; // Children iterators
  std::priority_queue<SABIInternalIterator*, std::vector<SABIInternalIterator*>,
                      IteratorComparator>
      heap_;

public:
  SABIMergingIterator(rocksdb::SuperVersion* sv, SABIOptions options,
                      SABIQuery query);
  ~SABIMergingIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

// Level Iterator for SST with SABI
class SABILevelIterator : public SABIInternalIterator {
private:
  // Table & query context
  uint32_t level_;
  const SABIOptions options_;
  const SABIQuery query_;
  rocksdb::SuperVersion* sv_;
  rocksdb::ColumnFamilyData* cfd_;
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
  SABILevelIterator(rocksdb::SuperVersion* sv, uint32_t level,
                    SABIOptions options, SABIQuery query);
  ~SABILevelIterator() override;
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

public:
  SABITableIterator(rocksdb::BlockBasedTable* bbt, SABIOptions options,
                    SABIQuery query);
  void SeekToFirst() override;
  void Next() override; // Get Next Data Entries
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

class SABIMemTableIterator : public SABIInternalIterator {
private:
  SABIOptions options_;
  rocksdb::MemTable* mem_;
  SABIQuery query_;

  // Internal status for iterating
  rocksdb::Arena arena_;
  rocksdb::InternalIterator* iter_ = nullptr;

  void FindNextValidEntry();

public:
  SABIMemTableIterator(rocksdb::MemTable* mem, SABIOptions options,
                       SABIQuery query);
  ~SABIMemTableIterator() override;

  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

} // namespace bitmap_index
