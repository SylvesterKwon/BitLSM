#pragma once
#include <bit_lsm_iterator.h>

#include <unordered_map>

#include "db/lookup_key.h"

namespace bit_lsm {

// A candidate row is "shadowed" when a newer version of its key exists
// somewhere the bitmap scan could not see -- a newer SST, a memtable, or a
// tombstone. Shadowed rows must not be served from the scan, which is why
// Verified mode re-fetches every candidate through MultiGet.
//
// ShadowChecker answers "may this candidate be shadowed?" using only the
// cheap prefix of a MultiGet descent: memtable probes plus per-file range /
// largest_seqno / bloom checks. It never reads a data block, and it never
// answers "clean" without proof -- anything it cannot rule out is reported as
// shadowed, so the caller falls back to the full MultiGet and a wrong answer
// can only cost performance.
//
// One blind spot is deliberate: the candidate's own source file is skipped,
// because its bloom filter always matches its own key. In-file shadowing is
// detected by the leaf iterator instead (see
// SABIInternalIterator::SourceHasNewerVersion).
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
