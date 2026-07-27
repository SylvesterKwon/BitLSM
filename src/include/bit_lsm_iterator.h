#pragma once

#include <bit_lsm_query.h>

#include <cstdint>
#include <deque>
#include <queue>

#include "bit_lsm_option.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"

namespace bit_lsm {

// Abstract class for internal iterator like SABITableIterator,
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

// What BitLSMIterator emits.
enum class ResultMode {
  // Bitmap pruning + authoritative MultiGet fetch + per-row verification:
  // only rows that truly match the query. The standalone default.
  Verified,
  // Bitmap pruning only: candidate keys, a superset of the answer at the
  // iterator's read seqno; the consumer fetches and re-verifies. Requires an
  // injected snapshot so candidate generation and the consumer's fetch see
  // the same seqno (a fresher seqno here would drop rows the consumer's
  // snapshot must still see).
  Candidate,
};

// "What to match": the query and its resolved forms, threaded to the leaf
// evaluators. Holds references into the owning BitLSMIterator, which outlives
// every child.
struct QueryContext {
  const BitLSMOptions& options;
  const SABIQuery& sabi_query;  // bitmap phase (SST skip, bin lookup)
  // Per-row verification; nullptr = yield every bitmap-phase candidate and
  // let the consumer re-verify. seqno/tombstone checks always stay on.
  const CompiledQuery* compiled;
};

// "Where to read": the version/snapshot backdrop a scan runs against, derived
// once from the SuperVersion and shared by the plumbing iterators (merging,
// level) instead of each re-deriving these fields.
struct ScanContext {
  rocksdb::SuperVersion* sv;
  rocksdb::TableCache* tc;
  const rocksdb::VersionStorageInfo* storage_info;
  const rocksdb::InternalKeyComparator* icmp;
  const rocksdb::MutableCFOptions& cf_opts;

  explicit ScanContext(rocksdb::SuperVersion* sv)
      : sv(sv),
        tc(sv->cfd->table_cache()),
        storage_info(sv->current->storage_info()),
        icmp(storage_info->InternalComparator()),
        cf_opts(sv->mutable_cf_options) {}
};

// Iterator for SABI
class BitLSMIterator : public SABIInternalIterator {
 private:
  rocksdb::DB* db_;
  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyHandle* cfh_;  // To use multiget API
  const rocksdb::Snapshot* snapshot_;
  // False when the snapshot was injected: the caller owns its lifetime and
  // this iterator must not release it.
  bool snapshot_owned_;
  rocksdb::ColumnFamilyData* cfd_;
  rocksdb::SuperVersion* sv_;

  BitLSMMergingIterator* smi_;
  BitLSMOptions options_;
  BitLSMQuery query_;
  ResultMode result_mode_;
  // In Candidate mode this is an inert default-constructed shell; the
  // nullable signal is query_ctx_.compiled, so never take &compiled_
  // unconditionally.
  CompiledQuery compiled_;
  SABIQuery sabi_query_;    // query_ encoded into the SABI domain
  QueryContext query_ctx_;  // references the members above
  ScanContext scan_ctx_;    // version backdrop, derived from sv_

  // batch
  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;

  // Dedup scratch per batch; a member so the vector's buffer survives. Its
  // elements move out into batch_keys_, so the strings are not reused.
  std::vector<std::string> candidate_keys_;

  std::string latest_user_key_added;

  void FetchNextBatch(uint32_t batch_size);

 public:
  // With the defaults (Verified mode, no injected snapshot) this behaves
  // exactly like the original standalone iterator. Candidate mode requires
  // an injected snapshot (see ResultMode).
  BitLSMIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfh,
                 const BitLSMOptions& options, const BitLSMQuery& query,
                 ResultMode result_mode = ResultMode::Verified,
                 const rocksdb::Snapshot* snapshot = nullptr);
  ~BitLSMIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice);  // Inspect Value for debug
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
  const ScanContext& scan_ctx_;
  std::vector<rocksdb::TableCache::TypedHandle*> l0_handles_;  // L0 handles

  // Internal status for iterating
  std::vector<SABIInternalIterator*> ch_iters_;  // Children iterators
  std::priority_queue<SABIInternalIterator*, std::vector<SABIInternalIterator*>,
                      IteratorComparator>
      heap_;

 public:
  BitLSMMergingIterator(const ScanContext& scan_ctx,
                        const QueryContext& query_ctx);
  ~BitLSMMergingIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice);  // Inspect Value for debug
};

// Level Iterator for SST with SABI
class BitLSMLevelIterator : public SABIInternalIterator {
 private:
  uint32_t level_;
  const ScanContext& scan_ctx_;
  const QueryContext& query_ctx_;
  const std::vector<rocksdb::FileMetaData*>& files_;

  // Internal status for iterating
  uint32_t cur_file_idx_;
  rocksdb::TableCache::TypedHandle* cur_table_handle_;
  SABITableIterator* cur_sti_;

  void LoadFile(size_t idx);  // Open file with index

 public:
  BitLSMLevelIterator(uint32_t level, const ScanContext& scan_ctx,
                      const QueryContext& query_ctx);
  ~BitLSMLevelIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice);  // Inspect Value for debug
};

// Table Iterator for SST with SABI
class SABITableIterator : public SABIInternalIterator {
 private:
  // Table & query context
  const BitLSMOptions& options_;
  rocksdb::BlockBasedTable* bbt_;
  // Pins the SABI block (and its parsed SABIReader) in the block cache for the
  // lifetime of this iterator; released on destruction so the entry becomes
  // evictable again. Declared before every member that references reader state
  // — members are destroyed in reverse order, so the pin must die last.
  rocksdb::CachableEntry<rocksdb::Block_kUserDefinedIndex> udi_entry_;
  SABIReader* sabi_reader_ = nullptr;  // points into udi_entry_; null on failure
  const SABIQuery& query_;
  const CompiledQuery* compiled_;  // nullptr = skip per-row Eval
  uint32_t block_restart_interval_ = 0;
  // Holds the current data block pinned in the Block Cache, so values can be
  // borrowed from it instead of copied.
  std::unique_ptr<rocksdb::DataBlockIter> biter_;

  // Internal status for iterating
  // The query bitmap is either borrowed from the SABIReader's frozen bitmaps
  // (the reader outlives this iterator via udi_entry_'s block cache pin) or
  // owned by bitmap_pool_. query_bitmap_ always points at the live bitmap.
  // bitmap_pool_ must remain a std::deque: element pointers (query_bitmap_,
  // BitmapRef::owned) must stay valid across emplace_back.
  std::deque<roaring::Roaring> bitmap_pool_;
  const roaring::Roaring* query_bitmap_;  // bitmap for current iteration
  roaring::Roaring::const_iterator bitmap_iter_;  // bitmap iterator
  roaring::Roaring::const_iterator bitmap_end_;
  std::vector<std::pair<uint32_t,
                        rocksdb::BlockHandle>>
      target_blocks_;                    // {index, blockhandle}
  int32_t cur_target_block_idx_ = -1;    // current index for target_blocks_
  std::vector<uint32_t> local_indexes_;  // buffer for block-local index
  // Slots are overwritten, never destroyed, so each keeps its buffer: size()
  // is capacity and buffer_count_ is the valid-entry count.
  std::vector<rocksdb::PinnableSlice> keys_buffer_;
  // Borrowed from the block biter_ pins; valid until the next block load.
  std::vector<rocksdb::Slice> values_buffer_;
  int32_t buffer_idx_ = 0;    // 버퍼 내 현재 커서
  int32_t buffer_count_ = 0;  // 버퍼 내 유효 엔트리 수

  // Get all data entries by indexes from data block
  // indexes must be sorted and unique
  void GetAllByIndexesFromDataBlock(
      const rocksdb::BlockHandle& bh, std::vector<uint32_t>& indexes,
      std::vector<rocksdb::PinnableSlice>& out_keys,
      std::vector<rocksdb::Slice>& out_values);

  // A bitmap that is either borrowed from the SABIReader (owned == nullptr)
  // or owned by bitmap_pool_ (owned points to the pool entry).
  struct BitmapRef {
    const roaring::Roaring* ptr;
    roaring::Roaring* owned;
  };
  // Get bitmap for a single SABICondition (leaf node in CNF)
  BitmapRef GetBitmapForSingleCondition(
      const SABICondition& cond, std::vector<const roaring::Roaring*>& buf);
  // Build bitmap for a full query (CNF: AND of OR clauses) into
  // query_bitmap_ (borrowing reader bitmaps where possible)
  void BuildQueryBitmap(const SABIQuery& query);
  // Fill buffer by loading next data block
  void LoadNextBlock();

 public:
  SABITableIterator(rocksdb::BlockBasedTable* bbt,
                    const QueryContext& query_ctx);
  void SeekToFirst() override;
  void Next() override;  // Get Next Data Entries
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice);  // Inspect Value for debug
};

class BitLSMMemTableIterator : public SABIInternalIterator {
 private:
  const BitLSMOptions& options_;
  rocksdb::MemTable* mem_;
  const CompiledQuery* compiled_;  // nullptr = skip per-row Eval

  // Internal status for iterating
  rocksdb::Arena arena_;
  rocksdb::InternalIterator* iter_ = nullptr;

  void FindNextValidEntry();

 public:
  BitLSMMemTableIterator(rocksdb::MemTable* mem, const QueryContext& query_ctx);
  ~BitLSMMemTableIterator() override;

  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  void TEST_DumpValue(rocksdb::Slice slice);  // Inspect Value for debug
};

}  // namespace bit_lsm
