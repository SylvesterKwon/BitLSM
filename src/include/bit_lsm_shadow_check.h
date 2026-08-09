#pragma once
#include <bit_lsm_iterator.h>

#include <unordered_map>

#include "db/lookup_key.h"

namespace bit_lsm {

// A candidate row is "shadowed" when a newer version of its key -- an update
// or a tombstone -- exists somewhere the per-file bitmap scan cannot see.
// Such rows must not be served from the scan, which is why Verified mode
// re-fetches candidates through MultiGet.
//
// ShadowChecker answers "may this candidate be shadowed?" from the cheap
// prefix of a MultiGet descent: memtable probes plus per-file range /
// largest_seqno / bloom checks, never a data block. It reports anything it
// cannot rule out as shadowed, so a wrong answer only costs a re-fetch.
//
// It deliberately ignores the candidate's own source file, whose bloom
// filter always matches its own key; the leaf reports that case through
// SABIInternalIterator::SourceHasNewerVersion.
class ShadowChecker {
 public:
  ShadowChecker(const ScanContext& scan_ctx,
                rocksdb::SequenceNumber read_seqno);
  ~ShadowChecker();  // releases cached table handles

  bool MayHaveNewerVersion(const rocksdb::Slice& user_key,
                           rocksdb::SequenceNumber cand_seqno, int src_level,
                           uint64_t src_file_no);

 private:
  bool MemtablesHaveNewer(const rocksdb::LookupKey& lk,
                          rocksdb::SequenceNumber cand_seqno);
  // True iff the file's bloom filter definitively excludes the key.
  bool FilterSaysNo(const rocksdb::FdWithKeyRange& f,
                    const rocksdb::Slice& user_key,
                    const rocksdb::Slice& internal_key);
  void ReleaseHandles();

  // Probing one key touches at most a handful of files; this bounds how many
  // readers stay pinned across a long scan.
  static constexpr size_t kMaxPinnedHandles = 64;

  const ScanContext& scan_ctx_;
  rocksdb::SequenceNumber read_seqno_;
  // Hoisted: ReadOptions is a large struct and the probes are per-candidate.
  rocksdb::ReadOptions read_options_;
  // file number -> pinned table cache handle, released in the destructor.
  std::unordered_map<uint64_t, rocksdb::TableCache::TypedHandle*> handles_;
};

}  // namespace bit_lsm
