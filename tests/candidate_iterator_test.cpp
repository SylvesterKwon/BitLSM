#include <gtest/gtest.h>
#include <rocksdb/db.h>

#include <set>
#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;

namespace {
std::set<std::string> CollectKeys(BitLSMIterator* it) {
  std::set<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    keys.insert(it->key().ToString());
  return keys;
}
}  // namespace

// pk2 does not satisfy the query but must still surface as a candidate.
TEST_F(BitLSMTestBase, CandidateModeSkipsRowVerification) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  const rocksdb::Snapshot* snap = db.GetInternalDB()->GetSnapshot();
  {
    auto it = db.NewIterator(query, ResultMode::Candidate, snap);
    ASSERT_NE(it, nullptr);
    // Memtable rows are not bitmap-pruned, so with verification off every
    // live row surfaces as a candidate, including non-matching pk2.
    EXPECT_EQ(CollectKeys(it.get()),
              (std::set<std::string>{"pk1", "pk2", "pk3"}));
  }
  db.GetInternalDB()->ReleaseSnapshot(snap);
}

// Rows split across an SST and the memtable, queried in both modes with
// the same injected snapshot.
TEST_F(BitLSMTestBase, VerifiedIsSubsetOfCandidatesAtSameSeqno) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));
  BITLSM_ASSERT_OK(db.Put("pk4", {30.0, std::string("cherry")}, "p4"));
  BITLSM_ASSERT_OK(db.Put("pk5", {7.0, std::string("apple")}, "p5"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  const rocksdb::Snapshot* snap = db.GetInternalDB()->GetSnapshot();
  std::set<std::string> verified, candidates;
  {
    auto it = db.NewIterator(query, ResultMode::Verified, snap);
    ASSERT_NE(it, nullptr);
    verified = CollectKeys(it.get());
  }
  {
    auto it = db.NewIterator(query, ResultMode::Candidate, snap);
    ASSERT_NE(it, nullptr);
    candidates = CollectKeys(it.get());
  }
  db.GetInternalDB()->ReleaseSnapshot(snap);

  EXPECT_EQ(verified, (std::set<std::string>{"pk1", "pk3", "pk4"}));
  for (const auto& k : verified)
    EXPECT_TRUE(candidates.count(k)) << "candidates lost true match " << k;
}

// Newer matching rows land after the injected snapshot and must not
// appear.
TEST_F(BitLSMTestBase, CandidateModeRespectsInjectedSeqno) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));

  const rocksdb::Snapshot* snap = db.GetInternalDB()->GetSnapshot();
  BITLSM_ASSERT_OK(db.Put("pk9", {99.0, std::string("apple")}, "p9"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  {
    auto it = db.NewIterator(query, ResultMode::Candidate, snap);
    ASSERT_NE(it, nullptr);
    EXPECT_EQ(CollectKeys(it.get()), (std::set<std::string>{"pk1"}));
  }
  db.GetInternalDB()->ReleaseSnapshot(snap);
}

// The caller keeps using its snapshot after the iterator is destroyed.
TEST_F(BitLSMTestBase, InjectedSnapshotSurvivesIterator) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));

  rocksdb::DB* rdb = db.GetInternalDB();
  const rocksdb::Snapshot* snap = rdb->GetSnapshot();
  rocksdb::ReadOptions ro;
  ro.snapshot = snap;
  std::string before;
  BITLSM_ASSERT_OK(rdb->Get(ro, "pk1", &before));

  BITLSM_ASSERT_OK(db.Put("pk1", {77.0, std::string("pear")}, "p1v2"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  {
    auto it = db.NewIterator(query, ResultMode::Candidate, snap);
    ASSERT_NE(it, nullptr);
    it->SeekToFirst();
  }  // iterator destroyed here; snap must stay alive

  std::string after;
  BITLSM_ASSERT_OK(rdb->Get(ro, "pk1", &after));
  EXPECT_EQ(before, after);
  rdb->ReleaseSnapshot(snap);
}

// The default CF holds decoy rows.
TEST_F(BitLSMTestBase, CandidateModeReadsRequestedColumnFamily) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("dk1", {15.0, std::string("apple")}, "p1"));

  ColumnFamilyHandle* aux = nullptr;
  BITLSM_ASSERT_OK(db.CreateColumnFamily("aux", DefaultOptions(), &aux));
  BITLSM_ASSERT_OK(db.Put(aux, "ak1", {15.0, std::string("apple")}, "pa"));
  BITLSM_ASSERT_OK(db.Put(aux, "ak2", {5.0, std::string("banana")}, "pb"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  const rocksdb::Snapshot* snap = db.GetInternalDB()->GetSnapshot();
  {
    auto it = db.NewIterator(aux, query, ResultMode::Candidate, snap);
    ASSERT_NE(it, nullptr);
    // Memtable rows of the requested CF, unverified, and none of the default
    // CF's.
    EXPECT_EQ(CollectKeys(it.get()), (std::set<std::string>{"ak1", "ak2"}));
  }
  db.GetInternalDB()->ReleaseSnapshot(snap);
}

// Without an injected snapshot candidates would be generated at a
// fresher seqno than the caller's fetch.
TEST_F(BitLSMTestBase, CandidateModeWithoutSnapshotRejected) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_THROW(db.NewIterator(query, ResultMode::Candidate,
                              /*snapshot=*/nullptr),
               std::invalid_argument);
}

// value() has nothing to return in Candidate mode. assert is compiled out
// in Release, so this must throw.
TEST_F(BitLSMTestBase, CandidateModeValueThrows) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  const rocksdb::Snapshot* snap = db.GetInternalDB()->GetSnapshot();
  {
    auto it = db.NewIterator(query, ResultMode::Candidate, snap);
    ASSERT_NE(it, nullptr);
    it->SeekToFirst();
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key().ToString(), "pk1");
    EXPECT_THROW(it->value(), std::logic_error);
  }
  db.GetInternalDB()->ReleaseSnapshot(snap);
}
