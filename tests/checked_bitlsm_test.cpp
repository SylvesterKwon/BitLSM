#include "test_util/checked_bitlsm.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"

using namespace bit_lsm;

// Same data as the FilteredQuery tests, driven through the checked wrapper.
TEST_F(BitLSMTestBase, CoreAgreesInMemtableAndAfterFlush) {
  BitLSMOptions opt = DefaultOptions();  // {kRange, kEquality}
  CheckedBitLSM db(&OpenDB(opt), opt);

  ASSERT_TRUE(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  ASSERT_TRUE(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  ASSERT_TRUE(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  ASSERT_TRUE(db.VerifyQuery(q));  // memtable path
  ASSERT_TRUE(db.VerifyFullScan());

  ASSERT_TRUE(db.Flush());  // memtable -> SST (SABI bitmap path)
  ASSERT_TRUE(db.VerifyQuery(q));
  ASSERT_TRUE(db.VerifyFullScan());
}

// A phantom key is injected into the reference only; VerifyFullScan must
// fail.
TEST_F(BitLSMTestBase, CheckerDetectsDivergence) {
  BitLSMOptions opt = DefaultOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);

  ASSERT_TRUE(db.Put("k1", {1.0, std::string("a")}, "p"));
  db.reference().Put("phantom", {2.0, std::string("b")}, "p");  // oracle-only

  EXPECT_FALSE(db.VerifyFullScan());  // must detect the missing phantom key
}
