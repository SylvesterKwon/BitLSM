#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

using namespace bit_lsm;

namespace {
// {kRange, kEquality} schema shared by these scenarios.
BitLSMQuery GeCont(double v) {
  return BitLSMQuery(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, v}});
}
BitLSMQuery EqCat(const std::string& v) {
  return BitLSMQuery(std::vector<QueryCondition>{{1, CompareOp::EQUAL, v}});
}
}  // namespace

TEST_F(BitLSMTestBase, OverwriteUpdatesAttributes) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {10.0, std::string("a")}, "p"));
  ASSERT_TRUE(db.Put("k1", {99.0, std::string("a")}, "p2"));  // overwrite
  ASSERT_TRUE(db.VerifyQuery(GeCont(50.0)));  // only the new value matches
  ASSERT_TRUE(db.VerifyFullScan());
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(GeCont(50.0)));
}

// Overwritten to not match, then back to match.
TEST_F(BitLSMTestBase, OverwriteFlipsQueryMatch) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {100.0, std::string("a")}, "p"));  // matches a0>=50
  ASSERT_TRUE(db.VerifyQuery(GeCont(50.0)));
  ASSERT_TRUE(db.Put("k1", {1.0, std::string("a")}, "p"));  // now excluded
  ASSERT_TRUE(db.VerifyQuery(GeCont(50.0)));
  ASSERT_TRUE(db.Flush());  // across flush too
  ASSERT_TRUE(db.Put("k1", {200.0, std::string("a")}, "p"));  // included again
  ASSERT_TRUE(db.VerifyQuery(GeCont(50.0)));
  ASSERT_TRUE(db.VerifyFullScan());
}

TEST_F(BitLSMTestBase, DeleteThenReinsert) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {10.0, std::string("a")}, "old"));
  ASSERT_TRUE(db.Delete("k1"));
  ASSERT_TRUE(db.Put("k1", {20.0, std::string("b")},
                     "new"));        // attrs differ from pre-delete
  ASSERT_TRUE(db.VerifyFullScan());  // exactly one k1 with the new record
  ASSERT_TRUE(db.VerifyQuery(EqCat("b")));  // new unordered visible
  ASSERT_TRUE(db.VerifyQuery(EqCat("a")));  // stale unordered not resurrected
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyFullScan());
  ASSERT_TRUE(db.VerifyQuery(EqCat("b")));
}

TEST_F(BitLSMTestBase, DeleteAbsentKeyIsNoop) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {10.0, std::string("a")}, "p"));
  ASSERT_TRUE(db.Delete("does-not-exist"));
  ASSERT_TRUE(db.VerifyFullScan());
}

TEST_F(BitLSMTestBase, DeleteAfterFlushShadowsSstKey) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {10.0, std::string("a")}, "p"));
  ASSERT_TRUE(db.Put("k2", {20.0, std::string("a")}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.Delete("k1"));
  ASSERT_TRUE(db.VerifyFullScan());  // tombstone in memtable shadows SST key
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyFullScan());  // tombstone now in its own SST
}

TEST_F(BitLSMTestBase, DeleteThenCompactStable) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 20; ++i) {
    std::string k = "k" + std::to_string(i);
    ASSERT_TRUE(db.Put(k, {static_cast<double>(i), std::string("a")}, "p"));
  }
  ASSERT_TRUE(db.Flush());
  for (int i = 0; i < 20; i += 2)
    ASSERT_TRUE(db.Delete("k" + std::to_string(i)));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.CompactAll());
  ASSERT_TRUE(db.VerifyFullScan());
  ASSERT_TRUE(db.VerifyQuery(GeCont(10.0)));
}

// Later in the batch wins.
TEST_F(BitLSMTestBase, PutBatchDuplicateAndOverwrite) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {1.0, std::string("a")}, "pre"));  // pre-existing
  std::vector<std::string> keys{"k1", "k2", "k2"};            // k2 duplicated
  std::vector<std::vector<Attr>> attrs{{10.0, std::string("a")},
                                       {20.0, std::string("b")},
                                       {30.0, std::string("b")}};
  std::vector<std::string> payloads{"p1", "p2", "p3"};
  ASSERT_TRUE(db.PutBatch(keys, attrs, payloads));
  ASSERT_TRUE(db.VerifyFullScan());  // k1=10, k2=30 (last wins)
  ASSERT_TRUE(db.VerifyQuery(EqCat("b")));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyFullScan());
}
