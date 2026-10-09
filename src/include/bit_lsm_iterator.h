#pragma once

#include <bit_lsm_query.h>

#include <cstdint>
#include <deque>
#include <queue>

#include "bit_lsm_option.h"
#include "block_prefetch_queue.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "file/readahead_file_info.h"
#include "rocksdb/status.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_prefetcher.h"

namespace bit_lsm {

// Source level of candidates from a memtable child.
inline constexpr int kMemtableSourceLevel = -1;

// Abstract class for internal iterator like SABITableIterator,
// BitLSMMemTableIterator
class SABIInternalIterator {
 protected:
  bool valid_ = false;
  // OK unless iteration hit an error; sticky. Every layer distinguishes
  // !Valid() with OK (exhausted) from !Valid() with !OK (stopped early).
  rocksdb::Status status_;

 public:
  virtual ~SABIInternalIterator() {};
  virtual void SeekToFirst() = 0;
  virtual void Next() = 0;
  bool Valid() const { return valid_; };
  virtual rocksdb::Status status() const { return status_; };
  virtual rocksdb::Slice key() const = 0;
  virtual rocksdb::Slice value() const = 0;
  // Where the current row came from; the shadow check bounds its search with
  // this. Memtable children keep the defaults.
  virtual int SourceLevel() const { return kMemtableSourceLevel; }
  virtual uint64_t SourceFileNumber() const { return 0; }
  // True when a newer version of the current row may live in its own source
  // file, which ShadowChecker does not search. Memtable children return
  // false.
  virtual bool SourceHasNewerVersion() const { return false; }
};

class BitLSMIterator;
class BitLSMMergingIterator;
class BitLSMLevelIterator;
class SABITableIterator;
class BitLSMMemTableIterator;
class ShadowChecker;

// What BitLSMIterator emits.
enum class ResultMode {
  // Bitmap pruning, MultiGet fetch and per-row verification: only matching
  // rows. The default.
  Verified,
  // Bitmap pruning only: candidate keys, a superset of the answer; the
  // consumer fetches and re-verifies. Requires an injected snapshot so both
  // sides see the same seqno.
  Candidate,
};

// The query and its resolved forms, shared by the leaf evaluators.
// References members of the owning BitLSMIterator.
struct QueryContext {
  const BitLSMOptions& options;
  const SABIQuery& sabi_query;  // bitmap phase (SST skip, bin lookup)
  // Per-row verification; nullptr yields every bitmap candidate.
  // seqno/tombstone checks always stay on.
  const CompiledQuery* compiled;
  // Whether leaves must compute SourceHasNewerVersion(); only the per-key
  // check consumes it.
  bool track_source_versions = false;
};

// The version a scan runs against, derived once from the SuperVersion and
// shared by the merging and level iterators.
struct ScanContext {
  rocksdb::SuperVersion* sv;
  rocksdb::TableCache* tc;
  const rocksdb::VersionStorageInfo* storage_info;
  const rocksdb::InternalKeyComparator* icmp;
  const rocksdb::MutableCFOptions& cf_opts;
  // CF-configured file options. Table opens must pass these: the table cache
  // keeps whichever reader the first opener created.
  const rocksdb::FileOptions& file_opts;

  explicit ScanContext(rocksdb::SuperVersion* sv)
      : sv(sv),
        tc(sv->cfd->table_cache()),
        storage_info(sv->current->storage_info()),
        icmp(storage_info->InternalComparator()),
        cf_opts(sv->mutable_cf_options),
        file_opts(*sv->cfd->soptions()) {}
};

// Top-level scan iterator. !Valid() alone means no more rows; a non-OK
// status() means the scan stopped on an error and the result is incomplete.
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
  // Unused in Candidate mode; query_ctx_.compiled is the nullable signal.
  CompiledQuery compiled_;
  SABIQuery sabi_query_;    // query_ encoded into the SABI domain
  QueryContext query_ctx_;  // references the members above
  ScanContext scan_ctx_;    // version backdrop, derived from sv_

  // batch
  std::vector<std::string> batch_keys_;
  std::vector<std::string> batch_values_;
  uint32_t batch_cur_idx_ = 0;

  // Dedup scratch per batch; elements move out into batch_keys_.
  std::vector<std::string> candidate_keys_;
  // Scan-row values for candidates that may skip the re-fetch. Copied, since
  // the source Slice dies with the child's next block.
  std::vector<std::string> candidate_values_;

  std::string latest_user_key_added;

  // True when the LSM shape alone proves every candidate is the newest
  // visible version of its key, so whole batches answer from the scan rows.
  bool authoritative_scan_ = false;
  uint64_t skipped_batches_ = 0;

  // Per-candidate inputs to the shadow check, indexed like candidate_keys_
  // and reused across batches the same way.
  std::vector<rocksdb::SequenceNumber> candidate_seqnos_;
  std::vector<int> candidate_src_levels_;
  std::vector<uint64_t> candidate_src_files_;
  std::vector<uint8_t> candidate_in_file_shadowed_;
  std::vector<uint32_t> dirty_idx_;
  std::unique_ptr<ShadowChecker> checker_;
  bool check_enabled_ = false;
  uint64_t checked_keys_ = 0;
  uint64_t skipped_keys_ = 0;

  void FetchNextBatch(uint32_t batch_size);

 public:
  // Candidate mode requires an injected snapshot (see ResultMode).
  BitLSMIterator(rocksdb::DB* db, rocksdb::ColumnFamilyHandle* cfh,
                 const BitLSMOptions& options, const BitLSMQuery& query,
                 ResultMode result_mode = ResultMode::Verified,
                 const rocksdb::Snapshot* snapshot = nullptr);
  ~BitLSMIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  // Skip telemetry: whole batches, then per-key.
  uint64_t TEST_SkippedBatches() const { return skipped_batches_; }
  uint64_t TEST_SkippedKeys() const { return skipped_keys_; }
  uint64_t TEST_CheckedKeys() const { return checked_keys_; }
  bool TEST_CheckEnabled() const { return check_enabled_; }
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

// Merges the memtable and level iterators.
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
  // Delegate to the child that owns the current row.
  int SourceLevel() const override { return heap_.top()->SourceLevel(); }
  uint64_t SourceFileNumber() const override {
    return heap_.top()->SourceFileNumber();
  }
  bool SourceHasNewerVersion() const override {
    return heap_.top()->SourceHasNewerVersion();
  }
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
  // Readahead state of the last file whose scan built a prefetch buffer,
  // handed to each newly opened file so the ramp survives file switches.
  rocksdb::ReadaheadFileInfo readahead_file_info_;

  void LoadFile(size_t idx);  // Open file with index

 public:
  BitLSMLevelIterator(uint32_t level, const ScanContext& scan_ctx,
                      const QueryContext& query_ctx);
  ~BitLSMLevelIterator() override;
  void SeekToFirst() override;
  void Next() override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  int SourceLevel() const override;  // delegates to cur_sti_
  uint64_t SourceFileNumber() const override;
  bool SourceHasNewerVersion() const override;
  void TEST_DumpValue(rocksdb::Slice slice);  // Inspect Value for debug
};

// Table Iterator for SST with SABI
class SABITableIterator : public SABIInternalIterator {
 private:
  // Table & query context
  const BitLSMOptions& options_;
  rocksdb::BlockBasedTable* bbt_;
  // Holds the SABI entry and its parsed SABIReader for this iterator's
  // lifetime: a block cache pin, or an unowned reference to the
  // table-lifetime pin. Declared first so it dies last.
  rocksdb::CachableEntry<rocksdb::Block_kUserDefinedIndex> udi_entry_;
  SABIReader* sabi_reader_ =
      nullptr;  // points into udi_entry_; null on failure
  const SABIQuery& query_;
  const CompiledQuery* compiled_;  // nullptr = skip per-row Eval
  // Whether to fill shadowed_buffer_ during the block walk.
  bool track_source_versions_ = false;
  uint32_t block_restart_interval_ = 0;
  // Position of this iterator's SST in the LSM.
  int source_level_ = 0;
  uint64_t file_number_ = 0;
  // Holds the current data block pinned in the Block Cache, so values can be
  // borrowed from it instead of copied.
  std::unique_ptr<rocksdb::DataBlockIter> biter_;
  // RocksDB's adaptive readahead for the data block reads prefetch_queue_
  // did not cover; it only engages once block accesses turn near-sequential.
  rocksdb::BlockPrefetcher block_prefetcher_;
  // Target-block reads kept in flight (block_prefetch_queue.h). Borrows bbt_,
  // so declared after it.
  BlockPrefetchQueue prefetch_queue_;
  // Overlaps this query's cold bin-run reads in metadata mode; null in
  // resident mode and once async reads are known unavailable. Borrows
  // sabi_reader_ and bbt_.
  std::unique_ptr<SABISpanPrefetch> span_prefetch_;

  // Internal status for iterating
  // Cached-bin pins for this scan; views borrowed via Bin() stay valid
  // until these release. Declared before bitmap_pool_ so it dies after it.
  std::vector<SABIPinnedBin> pinned_bins_;
  // The query bitmap is either borrowed from the SABIReader or owned by
  // bitmap_pool_. A deque, since element pointers must survive emplace_back.
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
  // Per-row flag: the preceding block entry has the same user key, so this
  // row is an older in-file version. An entry with no observable
  // predecessor is recorded as shadowed.
  std::vector<uint8_t> shadowed_buffer_;
  std::string prev_user_key_;
  int32_t buffer_idx_ = 0;    // cursor into the buffer
  int32_t buffer_count_ = 0;  // valid entries in the buffer

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
  // One bin's bitmap through the mode-resolving reader path; on failure
  // sets status_ and returns the empty bitmap.
  const roaring::Roaring* Bin(uint32_t flat_idx);
  const roaring::Roaring& Tombstone();
  // Fill buffer by loading next data block
  void LoadNextBlock();

 public:
  SABITableIterator(rocksdb::BlockBasedTable* bbt,
                    const QueryContext& query_ctx, int source_level,
                    uint64_t file_number);
  void SeekToFirst() override;
  void Next() override;  // Get Next Data Entries
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  int SourceLevel() const override { return source_level_; }
  uint64_t SourceFileNumber() const override { return file_number_; }
  bool SourceHasNewerVersion() const override {
    return shadowed_buffer_[buffer_idx_] != 0;
  }
  // Carry the readahead ramp across the files of a level scan, as RocksDB's
  // LevelIterator does.
  void GetReadaheadState(rocksdb::ReadaheadFileInfo* readahead_file_info);
  void SetReadaheadState(rocksdb::ReadaheadFileInfo* readahead_file_info);
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
