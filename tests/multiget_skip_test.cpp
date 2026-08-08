#include <gtest/gtest.h>
#include <rocksdb/filter_policy.h>

#include <string>
#include <utility>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

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

BitLSMQuery FullRange() {
  return BitLSMQuery(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 0.0}});
}

// Drains a Verified-mode scan; returns rows and reports how many batches the
// iterator answered from the scan alone (authoritative-scan skip).
std::vector<std::pair<std::string, std::string>> DrainWithSkipCount(
    BitLSM& db, uint64_t* skipped_batches) {
  BitLSMQuery q = FullRange();
  auto it = db.NewIterator(q);
  std::vector<std::pair<std::string, std::string>> rows;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    rows.emplace_back(it->key().ToString(), it->value().ToString());
  EXPECT_TRUE(it->status().ok());
  *skipped_batches = it->TEST_SkippedBatches();
  return rows;
}

void FillRows(BitLSM& db, int n, int start = 0) {
  const std::string payload(32, 'p');
  for (int i = start; i < start + n; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "k%05d", i);
    ASSERT_TRUE(db.Put(key, {static_cast<double>(i)}, payload).ok());
  }
}

void Settle(BitLSM& db) {
  ASSERT_TRUE(db.Flush().ok());
  rocksdb::CompactRangeOptions cro;
  // Force a bottommost rewrite: a trivial move keeps the original seqnos, and
  // the authoritative-scan check requires seqno-zeroed files.
  cro.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  ASSERT_TRUE(db.GetInternalDB()->CompactRange(cro, nullptr, nullptr).ok());
}

}  // namespace

// Workload: bulk load, Flush, full manual compaction (no live snapshots), then
//           a full-range Verified scan — the settled read-mostly state where
//           every key has exactly one, seqno-zeroed version.
// Threat: the iterator re-fetches every candidate through MultiGet even though
//         the scan already holds the authoritative row — the skip never
//         engages and settled scans keep paying the ~2x re-read tax.
TEST_F(BitLSMTestBase, SettledScanSkipsMultiGet) {
  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  FillRows(db, 5000);
  Settle(db);

  uint64_t skipped = 0;
  auto rows = DrainWithSkipCount(db, &skipped);
  ASSERT_EQ(rows.size(), 5000u);
  EXPECT_GT(skipped, 0u) << "settled scan still went through MultiGet";
}

// Workload: settled DB, then one Put lands in the memtable before the scan.
// Threat: the skip stays on with a non-empty memtable, so a candidate's newer
//         memtable version (here: an update that changes the payload) is
//         never consulted and the scan returns the stale compacted row.
TEST_F(BitLSMTestBase, MemtableWriteDisablesSkip) {
  BitLSMOptions opt = ContOpt();
  BitLSM& raw = OpenDB(opt);
  CheckedBitLSM db(&raw, opt);
  for (int i = 0; i < 1000; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.CompactAll());

  // Update one key in place; the new version lives only in the memtable.
  ASSERT_TRUE(db.Put("k500", {500.0}, "updated"));

  uint64_t skipped = 0;
  auto rows = DrainWithSkipCount(raw, &skipped);
  EXPECT_EQ(skipped, 0u) << "skip engaged with a non-empty memtable";
  ASSERT_TRUE(db.VerifyFullScan());
}

// Workload: settled DB plus one extra Flushed (uncompacted) SST, so L0 is
//           non-empty and the L0 file's seqnos are non-zero.
// Threat: the skip stays on despite L0 files, so newer L0 versions shadowing
//         compacted rows are never consulted.
TEST_F(BitLSMTestBase, L0FileDisablesSkip) {
  BitLSMOptions opt = ContOpt();
  BitLSM& raw = OpenDB(opt);
  CheckedBitLSM db(&raw, opt);
  for (int i = 0; i < 1000; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.CompactAll());

  // Update + Flush: the newer version now sits in an L0 SST.
  ASSERT_TRUE(db.Put("k500", {500.0}, "updated"));
  ASSERT_TRUE(db.Flush());

  uint64_t skipped = 0;
  auto rows = DrainWithSkipCount(raw, &skipped);
  EXPECT_EQ(skipped, 0u) << "skip engaged with a non-empty L0";
  ASSERT_TRUE(db.VerifyFullScan());
}

// Workload: two disjoint-range L0 flushes (bloom on), NO compaction — the
//           multi-level live state the OLTP benchmarks keep; full-range scan.
// Threat: v1's global condition can never hold here, so without per-key
//         checks every candidate still pays the MultiGet re-fetch.
TEST_F(BitLSMTestBase, MultiLevelCleanScanSkipsPerKey) {
  table_options_.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  FillRows(db, 1000, 0);
  ASSERT_TRUE(db.Flush().ok());
  FillRows(db, 1000, 1000);
  ASSERT_TRUE(db.Flush().ok());

  BitLSMQuery q = FullRange();
  auto it = db.NewIterator(q);
  std::vector<std::pair<std::string, std::string>> rows;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    rows.emplace_back(it->key().ToString(), it->value().ToString());
  ASSERT_TRUE(it->status().ok());
  ASSERT_EQ(rows.size(), 2000u);
  EXPECT_EQ(it->TEST_SkippedBatches(), 0u);  // v1 path must NOT be active
  EXPECT_EQ(it->TEST_SkippedKeys(), 2000u)
      << "clean multi-level candidates still re-fetched";
  EXPECT_EQ(it->TEST_CheckedKeys(), 2000u);
}

// Workload: 1000 rows flushed, then 100 of them updated OUT of the query
//           range and flushed again (newer shadowing L0 file); query
//           attr <= 1999 sees the old versions as bitmap candidates.
// Threat: a skipped stale candidate resurfaces a row whose newest version no
//         longer matches — the exact bug the MultiGet re-fetch existed for.
TEST_F(BitLSMTestBase, UpdatedKeysFallBackToMultiGet) {
  table_options_.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
  BitLSMOptions opt = ContOpt();
  BitLSM& raw = OpenDB(opt);
  CheckedBitLSM db(&raw, opt);
  for (int i = 0; i < 1000; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 0; i < 100; ++i)  // move out of the query's range
    ASSERT_TRUE(db.Put("k" + std::to_string(i * 10), {5000.0}, "u"));
  ASSERT_TRUE(db.Flush());

  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::LESS_EQUAL, 1999.0}});
  auto it = raw.NewIterator(q);
  uint64_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  ASSERT_TRUE(it->status().ok());
  EXPECT_EQ(n, 900u) << "stale shadowed rows leaked into the result";
  EXPECT_GT(it->TEST_SkippedKeys(), 0u);  // untouched keys skip
  EXPECT_LT(it->TEST_SkippedKeys(), it->TEST_CheckedKeys());
  ASSERT_TRUE(db.VerifyQuery(q));
}

// Workload: update + delete traffic, then a second full compaction settles
//           the DB again before the scan.
// Threat: after re-settling, either the skip fails to re-engage (perf loss)
//         or it returns pre-update rows / resurrects the deleted key
//         (correctness loss).
TEST_F(BitLSMTestBase, ResettledAfterUpdatesSkipsAgain) {
  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  FillRows(db, 1000);
  Settle(db);

  const std::string new_payload(16, 'u');
  ASSERT_TRUE(db.Put("k00500", {500.0}, new_payload).ok());
  ASSERT_TRUE(db.Delete("k00777").ok());
  Settle(db);

  uint64_t skipped = 0;
  auto rows = DrainWithSkipCount(db, &skipped);
  EXPECT_GT(skipped, 0u) << "re-settled scan still went through MultiGet";
  ASSERT_EQ(rows.size(), 999u);  // one key deleted
  bool found_updated = false, found_deleted = false;
  for (auto& [k, v] : rows) {
    if (k == "k00500") {
      found_updated = true;
      EXPECT_TRUE(v.find(new_payload) != std::string::npos)
          << "stale pre-update payload returned";
    }
    if (k == "k00777") found_deleted = true;
  }
  EXPECT_TRUE(found_updated);
  EXPECT_FALSE(found_deleted) << "deleted key resurrected";
}
