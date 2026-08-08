#include <gtest/gtest.h>
#include <rocksdb/filter_policy.h>

#include <string>

#include "bit_lsm_shadow_check.h"
#include "db/db_impl/db_impl.h"
#include "test_util/bitlsm_test_base.h"

using namespace bit_lsm;

namespace {

BitLSMOptions ContOpt() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec{AttrRole::ORDERED}};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}

// Pins a SuperVersion + ScanContext for direct ShadowChecker tests and
// releases them the way BitLSMIterator's destructor does.
struct SvGuard {
  rocksdb::DBImpl* impl;
  rocksdb::SuperVersion* sv;
  explicit SvGuard(BitLSM& db)
      : impl(static_cast<rocksdb::DBImpl*>(db.GetInternalDB())),
        sv(impl->GetVersionSet()
               ->GetColumnFamilySet()
               ->GetDefault()
               ->GetReferencedSuperVersion(impl)) {}
  ~SvGuard() {
    if (sv->Unref()) {
      impl->mutex()->Lock();
      sv->Cleanup();
      impl->mutex()->Unlock();
      delete sv;
    }
  }
};

// L0 file numbers by flush recency: LevelFiles(0) is newest-first.
uint64_t L0File(const SvGuard& g, size_t idx) {
  return g.sv->current->storage_info()->LevelFiles(0)[idx]->fd.GetNumber();
}

}  // namespace

// Workload: two overlapping-range L0 flushes (bloom on) — F_old holds a,m,z
//           (seqnos 1-3), F_new holds b,y (seqnos 4-5).
// Threat: the checker answers "clean" for a key whose newer version exists in
//         an upper file (stale row served as authoritative), or "dirty" for
//         keys provably absent above (kills the optimization).
TEST_F(BitLSMTestBase, ShadowCheckerVerdicts) {
  table_options_.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  ASSERT_TRUE(db.Put("a", {1.0}, "p").ok());  // seq 1
  ASSERT_TRUE(db.Put("m", {2.0}, "p").ok());  // seq 2
  ASSERT_TRUE(db.Put("z", {3.0}, "p").ok());  // seq 3
  ASSERT_TRUE(db.Flush().ok());               // F_old
  ASSERT_TRUE(db.Put("b", {4.0}, "p").ok());  // seq 4
  ASSERT_TRUE(db.Put("y", {5.0}, "p").ok());  // seq 5
  ASSERT_TRUE(db.Flush().ok());               // F_new

  SvGuard g(db);
  ScanContext ctx(g.sv);
  ShadowChecker checker(ctx, g.impl->GetLatestSequenceNumber());
  uint64_t f_new = L0File(g, 0), f_old = L0File(g, 1);

  // "a" from F_old: F_new's range [b,y] does not cover it -> clean.
  EXPECT_FALSE(checker.MayHaveNewerVersion("a", 1, 0, f_old));
  // "m" from F_old: covered by F_new, newer, bloom says absent -> clean.
  EXPECT_FALSE(checker.MayHaveNewerVersion("m", 2, 0, f_old));
  // "b" from F_new: F_old is older (largest_seqno 3 <= 4) -> clean.
  EXPECT_FALSE(checker.MayHaveNewerVersion("b", 4, 0, f_new));
  // Same key, pretending an older version at seqno 1 existed below: F_new
  // covers "b", is newer, bloom hits -> dirty.
  EXPECT_TRUE(checker.MayHaveNewerVersion("b", 1, 0, f_old));
}

// Workload: one flushed file, then updates land in the active memtable.
// Threat: memtable versions newer than the candidate are missed, so a stale
//         SST row would be served as authoritative.
TEST_F(BitLSMTestBase, ShadowCheckerSeesMemtable) {
  table_options_.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  ASSERT_TRUE(db.Put("a", {1.0}, "p").ok());  // seq 1
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Put("a", {9.0}, "p2").ok());  // seq 2, memtable only
  ASSERT_TRUE(db.Put("d", {4.0}, "p").ok());   // seq 3, memtable only

  SvGuard g(db);
  ScanContext ctx(g.sv);
  ShadowChecker checker(ctx, g.impl->GetLatestSequenceNumber());
  uint64_t f_old = L0File(g, 0);

  EXPECT_TRUE(checker.MayHaveNewerVersion("a", 1, 0, f_old));
  // Memtable-sourced candidate at its own (newest) seqno: clean.
  EXPECT_FALSE(checker.MayHaveNewerVersion("d", 3, kMemtableSourceLevel, 0));
}

// Workload: the two-file layout from ShadowCheckerVerdicts but with no bloom
//           filter configured.
// Threat: without a filter the checker guesses "clean" for covered keys
//         instead of degrading to "maybe" — losing correctness instead of
//         performance.
TEST_F(BitLSMTestBase, ShadowCheckerBloomlessIsConservative) {
  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  ASSERT_TRUE(db.Put("a", {1.0}, "p").ok());
  ASSERT_TRUE(db.Put("m", {2.0}, "p").ok());
  ASSERT_TRUE(db.Put("z", {3.0}, "p").ok());
  ASSERT_TRUE(db.Flush().ok());
  ASSERT_TRUE(db.Put("b", {4.0}, "p").ok());
  ASSERT_TRUE(db.Put("y", {5.0}, "p").ok());
  ASSERT_TRUE(db.Flush().ok());

  SvGuard g(db);
  ScanContext ctx(g.sv);
  ShadowChecker checker(ctx, g.impl->GetLatestSequenceNumber());
  uint64_t f_old = L0File(g, 1);

  // Covered by the newer file and no filter to prove absence -> dirty.
  EXPECT_TRUE(checker.MayHaveNewerVersion("m", 2, 0, f_old));
  // Range check alone still clears keys outside the newer file's span.
  EXPECT_FALSE(checker.MayHaveNewerVersion("a", 1, 0, f_old));
}
