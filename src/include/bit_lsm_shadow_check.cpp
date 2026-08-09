#include "bit_lsm_shadow_check.h"

#include "db/lookup_key.h"
#include "db/memtable_list.h"
#include "db/version_set.h"
#include "table/block_based/block_based_table_reader.h"

using namespace rocksdb;

namespace bit_lsm {

ShadowChecker::ShadowChecker(const ScanContext& scan_ctx,
                             SequenceNumber read_seqno)
    : scan_ctx_(scan_ctx), read_seqno_(read_seqno) {}

ShadowChecker::~ShadowChecker() { ReleaseHandles(); }

void ShadowChecker::ReleaseHandles() {
  TableCache::CacheInterface cache = scan_ctx_.tc->get_cache();
  for (auto& [num, handle] : handles_) cache.Release(handle);
  handles_.clear();
}

bool ShadowChecker::MemtablesHaveNewer(const LookupKey& lk,
                                       SequenceNumber cand_seqno) {
  std::string value;
  Status s;
  MergeContext mc;
  SequenceNumber mcts = 0;
  SequenceNumber seq = kMaxSequenceNumber;
  // A hit of any kind (value or tombstone) newer than the candidate means the
  // candidate is stale or shadowed and must go through MultiGet. A covering
  // range tombstone counts too; it is reported through
  // max_covering_tombstone_seq rather than as a point hit.
  bool found =
      scan_ctx_.sv->mem->Get(lk, &value, /*columns=*/nullptr,
                             /*timestamp=*/nullptr, &s, &mc, &mcts, &seq,
                             read_options_, /*immutable_memtable=*/false);
  if (mcts > cand_seqno) return true;
  if (found && seq > cand_seqno) return true;

  value.clear();
  mc.Clear();
  mcts = 0;
  seq = kMaxSequenceNumber;
  s = Status();
  found = scan_ctx_.sv->imm->Get(lk, &value, /*columns=*/nullptr,
                                 /*timestamp=*/nullptr, &s, &mc, &mcts, &seq,
                                 read_options_);
  if (mcts > cand_seqno) return true;
  return found && seq > cand_seqno;
}

bool ShadowChecker::FilterSaysNo(const FdWithKeyRange& f, const Slice& user_key,
                                 const Slice& internal_key) {
  auto it = handles_.find(f.fd.GetNumber());
  if (it == handles_.end()) {
    // Keeping every probed reader pinned would make a wide, long-running scan
    // hold the whole table cache; drop the set once it grows past what a
    // single key's probe sequence needs.
    if (handles_.size() >= kMaxPinnedHandles) ReleaseHandles();
    TableCache::TypedHandle* handle = nullptr;
    Status s = scan_ctx_.tc->FindTable(read_options_, scan_ctx_.file_opts,
                                       *scan_ctx_.icmp, *f.file_metadata,
                                       &handle, scan_ctx_.cf_opts);
    if (!s.ok()) return false;  // cannot prove absence -> maybe
    it = handles_.emplace(f.fd.GetNumber(), handle).first;
  }
  auto* bbt = static_cast<BlockBasedTable*>(
      scan_ctx_.tc->get_cache().Value(it->second));
  const auto* rep = bbt->get_rep();
  if (rep->filter == nullptr || !rep->whole_key_filtering) return false;
  return !rep->filter->KeyMayMatch(user_key, &internal_key,
                                   /*get_context=*/nullptr,
                                   /*lookup_context=*/nullptr, read_options_);
}

bool ShadowChecker::MayHaveNewerVersion(const Slice& user_key,
                                        SequenceNumber cand_seqno,
                                        int src_level, uint64_t src_file_no) {
  LookupKey lk(user_key, read_seqno_);
  if (MemtablesHaveNewer(lk, cand_seqno)) return true;
  if (src_level == kMemtableSourceLevel) return false;  // nothing else above

  const VersionStorageInfo* vsi = scan_ctx_.storage_info;
  Slice ikey = lk.internal_key();
  const Comparator* ucmp = scan_ctx_.icmp->user_comparator();

  // L0: files overlap freely, so every one gets a range/seqno/bloom check.
  const LevelFilesBrief& l0 = vsi->LevelFilesBrief(0);
  for (size_t i = 0; i < l0.num_files; ++i) {
    const FdWithKeyRange& f = l0.files[i];
    if (f.fd.largest_seqno <= cand_seqno) continue;
    if (f.fd.GetNumber() == src_file_no) continue;
    if (ucmp->Compare(user_key, ExtractUserKey(f.smallest_key)) < 0 ||
        ucmp->Compare(user_key, ExtractUserKey(f.largest_key)) > 0) {
      continue;
    }
    // A range tombstone can cover this key without any point entry for it,
    // and the point-key bloom cannot see range deletions -- so a file that
    // carries any is never provably clean.
    if (f.file_metadata->num_range_deletions > 0) return true;
    if (FilterSaysNo(f, user_key, ikey)) continue;
    return true;
  }

  // L1..src_level: per-level files are disjoint, so at most one covers the
  // key (binary search). Levels below src_level cannot hold a newer version
  // (LSM invariant), and at src_level itself the covering file can only be
  // the source file, which the file-number check excludes.
  for (int level = 1; level <= src_level && level < vsi->num_non_empty_levels();
       ++level) {
    const LevelFilesBrief& brief = vsi->LevelFilesBrief(level);
    if (brief.num_files == 0) continue;
    int idx = FindFile(*scan_ctx_.icmp, brief, ikey);
    if (idx >= static_cast<int>(brief.num_files)) continue;
    const FdWithKeyRange& f = brief.files[idx];
    if (ucmp->Compare(user_key, ExtractUserKey(f.smallest_key)) < 0) continue;
    if (f.fd.largest_seqno <= cand_seqno) continue;
    if (f.fd.GetNumber() == src_file_no) continue;
    // A range tombstone can cover this key without any point entry for it,
    // and the point-key bloom cannot see range deletions -- so a file that
    // carries any is never provably clean.
    if (f.file_metadata->num_range_deletions > 0) return true;
    if (FilterSaysNo(f, user_key, ikey)) continue;
    return true;
  }
  return false;
}

}  // namespace bit_lsm
