#pragma once

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"

#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include <bit_lsm_query.h>
#include <cstdint>
#include <queue>

namespace bit_lsm {

// Abstract class for internal iterator SABITableIterator,
// BitLSMMemTableIterator
class SABIInternalIterator {
protected:
  bool valid_ = false;

public:
  virtual ~SABIInternalIterator() {};
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  bool Valid() const { return valid_; };
  virtual rocksdb::Slice key() const = 0;
  virtual rocksdb::Slice value() const = 0;
};

class BitLSMIterator;
class BitLSMMergingIterator;
class BitLSMLevelIterator;
class SABITableIterator;
class BitLSMMemTableIterator;

// Iterator for SABI
class BitLSMIterator : public SABIInternalIterator {
private:
  rocksdb::DB* db_;
  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyHandle* cfh_; // To use multiget API
  const rocksdb::Snapshot* snapshot_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::SuperVersion* sv_;

  BitLSMMergingIterator* smi_;
  BitLSMOptions options_;
  BitLSMQuery query_;

  // batch
  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;

  std::string latest_user_key_added;

  void FetchNextBatch(uint32_t batch_size);

public:
  BitLSMIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfh,
                 BitLSMOptions options, BitLSMQuery query);
  ~BitLSMIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

// Iterator comparator for BitLSMMergingIterator
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
class BitLSMMergingIterator : public SABIInternalIterator {
private:
  // Table & query context
  const BitLSMOptions options_;
  const BitLSMQuery query_;
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
  BitLSMMergingIterator(rocksdb::SuperVersion* sv, BitLSMOptions options,
                        BitLSMQuery query);
  ~BitLSMMergingIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

// Level Iterator for SST with SABI
class BitLSMLevelIterator : public SABIInternalIterator {
private:
  // Table & query context
  uint32_t level_;
  const BitLSMOptions options_;
  const BitLSMQuery query_;
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
  BitLSMLevelIterator(rocksdb::SuperVersion* sv, uint32_t level,
                      BitLSMOptions options, BitLSMQuery query);
  ~BitLSMLevelIterator() override;
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
  BitLSMOptions options_;
  rocksdb::BlockBasedTable* bbt_;
  rocksdb::BlockBasedTable::IndexReader* index_reader_;
  SABIReader* sabi_reader_;
  BitLSMQuery query_;
  uint32_t block_restart_interval_;
  // Promoted to a member variable to enable Zero-copy evaluation.
  // Keeping this iterator alive ensures the underlying data block remains
  // pinned in the Block Cache, allowing us to safely use PinSlice() for values.
  std::unique_ptr<rocksdb::DataBlockIter> biter_;

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
  roaring::Roaring GetBitmapFromQuery(const BitLSMQuery& query);
  // Fill buffer by loading next data block
  void LoadNextBlock();

public:
  SABITableIterator(rocksdb::BlockBasedTable* bbt, BitLSMOptions options,
                    BitLSMQuery query);
  void SeekToFirst() override;
  void Next() override; // Get Next Data Entries
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

class BitLSMMemTableIterator : public SABIInternalIterator {
private:
  BitLSMOptions options_;
  rocksdb::MemTable* mem_;
  BitLSMQuery query_;

  // Internal status for iterating
  rocksdb::Arena arena_;
  rocksdb::InternalIterator* iter_ = nullptr;

  void FindNextValidEntry();

public:
  BitLSMMemTableIterator(rocksdb::MemTable* mem, BitLSMOptions options,
                         BitLSMQuery query);
  ~BitLSMMemTableIterator() override;

  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice); // Inspect Value for debug
};

} // namespace bit_lsm
