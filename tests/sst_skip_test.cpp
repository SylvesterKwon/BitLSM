#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

using namespace bit_lsm;

namespace {

BitLSMOptions ContOpt() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_types = {AttrType::CONTINUOUS};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}
BitLSMOptions CatOpt() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_types = {AttrType::CATEGORICAL};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}

BitLSMQuery ContQ(CompareOp op, double v) {
  return BitLSMQuery(std::vector<QueryCondition>{{0, op, v}});
}
BitLSMQuery CatEq(const std::string& v) {
  return BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::EQUAL, v}});
}

}  // namespace

// Workload: two Flushed SSTables with disjoint continuous ranges [0,9] and
//           [90,99]; query with a range that overlaps neither SST.
// Threat: SST-level skip incorrectly prunes a row that does match, or
//         incorrectly skips an SST that has matching rows — result diverges
//         from the reference oracle.
TEST_F(BitLSMTestBase, ContinuousSkip_BothSSTsMiss) {
  BitLSMOptions opt = ContOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 90; i < 100; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  // Query range [50, 80] overlaps neither SST → should return 0 rows.
  ASSERT_TRUE(db.VerifyQuery(
      BitLSMQuery(std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 50.0}},
                                        {{0, CompareOp::LESS_EQUAL, 80.0}}})));
}

// Workload: two Flushed SSTables with disjoint continuous ranges [0,9] and
//           [90,99]; query with attr>=5 matches part of SST1 and all of SST2.
// Threat: SST-level skip wrongly discards SST1 (min=0 ≤ 5 ≤ max=9) or SST2.
TEST_F(BitLSMTestBase, ContinuousSkip_PartialOverlap) {
  BitLSMOptions opt = ContOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 90; i < 100; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::GREATER_EQUAL, 5.0)));
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::GREATER_EQUAL, 0.0)));
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::LESS_EQUAL, 99.0)));
}

// Workload: two Flushed SSTables [0,9] and [90,99]; query exactly on SST1's
//           max boundary value (9).
// Threat: off-by-one at the skip boundary — GREATER_EQUAL(9) must NOT skip
//         SST1 (9 is in [0,9]); GREATER(9) must skip SST1 because no row
//         has attr > 9 in SST1.
TEST_F(BitLSMTestBase, ContinuousSkip_BoundaryAtSSTMax) {
  BitLSMOptions opt = ContOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 90; i < 100; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  // attr >= 9: row 9 from SST1 + all 10 from SST2 = 11
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::GREATER_EQUAL, 9.0)));
  // attr > 9: 0 from SST1, all 10 from SST2 = 10
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::GREATER, 9.0)));
  // attr <= 0: row 0 from SST1, 0 from SST2 = 1
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::LESS_EQUAL, 0.0)));
  // attr < 0: 0 rows
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::LESS, 0.0)));
}

// Workload: two Flushed SSTables [0,9] and [90,99]; EQUAL queries at various
//           values including one present in SST1, one in SST2, and one absent.
// Threat: EQUAL on a value below the SST min or above the SST max must skip;
//         EQUAL on the exact min/max must not skip.
TEST_F(BitLSMTestBase, ContinuousSkip_EqualAtBoundaries) {
  BitLSMOptions opt = ContOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 90; i < 100; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::EQUAL, 0.0)));   // SST1 min
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::EQUAL, 9.0)));   // SST1 max
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::EQUAL, 90.0)));  // SST2 min
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::EQUAL, 99.0)));  // SST2 max
  ASSERT_TRUE(
      db.VerifyQuery(ContQ(CompareOp::EQUAL, 50.0)));  // present in neither
  ASSERT_TRUE(
      db.VerifyQuery(ContQ(CompareOp::EQUAL, -1.0)));  // below both SSTs
  ASSERT_TRUE(
      db.VerifyQuery(ContQ(CompareOp::EQUAL, 100.0)));  // above both SSTs
}

// Workload: two Flushed SSTables [0,9] and [90,99]; a CNF query with two
//           AND-clauses, one of which is unsatisfiable against SST1.
// Threat: CNF short-circuit (skip if any clause is impossible) discards the
//         wrong SST or fails to discard the right one.
TEST_F(BitLSMTestBase, ContinuousSkip_CnfClauseElimination) {
  BitLSMOptions opt = ContOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 90; i < 100; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  // (attr >= 80) AND (attr <= 99): only SST2 can match
  ASSERT_TRUE(db.VerifyQuery(
      BitLSMQuery(std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 80.0}},
                                        {{0, CompareOp::LESS_EQUAL, 99.0}}})));
  // (attr <= 9) AND (attr >= 0): only SST1 can match
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(
      std::vector<OrClause>{{{0, CompareOp::LESS_EQUAL, 9.0}},
                            {{0, CompareOp::GREATER_EQUAL, 0.0}}})));
  // Contradictory CNF: (attr > 9) AND (attr < 9) → no SST can match
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(std::vector<OrClause>{
      {{0, CompareOp::GREATER, 9.0}}, {{0, CompareOp::LESS, 9.0}}})));
}

// Workload: two Flushed SSTables with disjoint categorical values ("apple" and
//           "banana"); EQUAL queries for present, absent, and cross-SST values.
// Threat: categorical skip incorrectly prunes an SST that contains the queried
//         value, or fails to prune one that does not.
TEST_F(BitLSMTestBase, CategoricalSkip_EqualPresentAndAbsent) {
  BitLSMOptions opt = CatOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 5; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {std::string("apple")}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 0; i < 5; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {std::string("banana")}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(CatEq("apple")));   // only SST1
  ASSERT_TRUE(db.VerifyQuery(CatEq("banana")));  // only SST2
  ASSERT_TRUE(db.VerifyQuery(CatEq("cherry")));  // neither SST → 0 rows
}

// Workload: three consecutive Flushes ([0,9], [10,19], [20,29]); full-scan
//           query and a mid-range query spanning exactly one SST.
// Threat: SST-level skip with >2 SSTables selects wrong subset.
TEST_F(BitLSMTestBase, ContinuousSkip_ThreeSSTsFullAndPartial) {
  BitLSMOptions opt = ContOpt();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("a" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 10; i < 20; ++i)
    ASSERT_TRUE(db.Put("b" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 20; i < 30; ++i)
    ASSERT_TRUE(db.Put("c" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyFullScan());  // all 30 rows
  ASSERT_TRUE(
      db.VerifyQuery(ContQ(CompareOp::GREATER_EQUAL, 10.0)));  // 20 rows
  ASSERT_TRUE(
      db.VerifyQuery(ContQ(CompareOp::LESS, 10.0)));  // 10 rows (SST1 only)
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::EQUAL, 15.0)));     // 1 row
  ASSERT_TRUE(db.VerifyQuery(ContQ(CompareOp::GREATER, 100.0)));  // 0 rows
}
