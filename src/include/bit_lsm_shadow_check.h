#pragma once
#include <bit_lsm_iterator.h>

#include <unordered_map>

namespace bit_lsm {

// Answers "may a newer visible version of this key exist above the
// candidate's position?" using only the cheap prefix of a MultiGet descent:
// memtable probes plus per-file range/seqno/bloom checks. Never touches data
// blocks; a "maybe" (true) is always safe because the caller falls back to
// the full MultiGet.
class ShadowChecker {
 public:
  ShadowChecker(const ScanContext& scan_ctx,
                rocksdb::SequenceNumber read_seqno);
  ~ShadowChecker();  // releases cached table handles

  bool MayHaveNewerVersion(const rocksdb::Slice& user_key,
                           rocksdb::SequenceNumber cand_seqno, int src_level,
                           uint64_t src_file_no);

 private:
  bool MemtablesHaveNewer(const rocksdb::Slice& user_key,
                          rocksdb::SequenceNumber cand_seqno);
  // True iff the file's bloom filter definitively excludes the key.
  bool FilterSaysNo(const rocksdb::FdWithKeyRange& f,
                    const rocksdb::Slice& user_key,
                    const rocksdb::Slice& internal_key);

  const ScanContext& scan_ctx_;
  rocksdb::SequenceNumber read_seqno_;
  // file number -> pinned table cache handle, released in the destructor.
  std::unordered_map<uint64_t, rocksdb::TableCache::TypedHandle*> handles_;
};

}  // namespace bit_lsm
