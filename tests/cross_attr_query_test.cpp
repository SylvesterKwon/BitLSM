#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

using namespace bit_lsm;

namespace {
// {ORDERED, UNORDERED, ORDERED} schema so OR clauses can cross
// attributes of the same type (a0/a2) and of mixed types (a0/a1).
BitLSMOptions ThreeAttrOptions() {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_types = {AttrType::ORDERED, AttrType::UNORDERED, AttrType::ORDERED};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}
}  // namespace

// Workload: rows where the cross-attr clause (a0>=50 OR a2>=50) matches only
//           via the SECOND condition's attribute; verify in memtable, after
//           Flush (SST bitmap + revalidation), and after CompactAll.
// Threat: CheckCondition decodes only clause[0]'s attribute and evaluates the
//         whole clause against it, silently dropping rows that match via a
//         different attribute (F1) on all three call sites.
TEST_F(BitLSMTestBase, CrossAttrOrClauseSameType) {
  BitLSMOptions opt = ThreeAttrOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  // k1 matches ONLY via a2; k2 matches ONLY via a0; k3 matches neither.
  ASSERT_TRUE(db.Put("k1", {1.0, std::string("x"), 100.0}, "p1"));
  ASSERT_TRUE(db.Put("k2", {99.0, std::string("y"), 1.0}, "p2"));
  ASSERT_TRUE(db.Put("k3", {1.0, std::string("z"), 1.0}, "p3"));
  BitLSMQuery q(std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 50.0},
                                       {2, CompareOp::GREATER_EQUAL, 50.0}}});
  ASSERT_TRUE(db.VerifyQuery(q));  // memtable path
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(q));  // SST bitmap + revalidation path
  ASSERT_TRUE(db.CompactAll());
  ASSERT_TRUE(db.VerifyQuery(q));
  ASSERT_TRUE(db.VerifyFullScan());
}

// Workload: cross-attr clause mixing types (a0>=50 OR a1='x'); the row matches
//           only via the unordered condition.
// Threat: pre-fix CheckCondition decodes a0 (double) and evaluates the
//         unordered condition against that double -> bad_variant_access.
TEST_F(BitLSMTestBase, CrossAttrOrClauseMixedType) {
  BitLSMOptions opt = ThreeAttrOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  ASSERT_TRUE(db.Put("k1", {1.0, std::string("x"), 2.0}, "p1"));
  ASSERT_TRUE(db.Put("k2", {1.0, std::string("y"), 2.0}, "p2"));
  BitLSMQuery q(
      std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 50.0},
                             {1, CompareOp::EQUAL, std::string("x")}}});
  ASSERT_TRUE(db.VerifyQuery(q));  // memtable path
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(q));  // SST path
}

// Workload: same-attr OR clause (a0<3 OR a0>17) after the fix.
// Threat: regression — the per-condition decode cache must not change
//         single-attr clause semantics (this worked before the fix).
TEST_F(BitLSMTestBase, SameAttrOrClauseStillWorks) {
  BitLSMOptions opt = ThreeAttrOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       {static_cast<double>(i), std::string("c"), 0.0}, "p"));
  }
  BitLSMQuery q(std::vector<OrClause>{
      {{0, CompareOp::LESS, 3.0}, {0, CompareOp::GREATER, 17.0}}});
  ASSERT_TRUE(db.VerifyQuery(q));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(q));
}

// Workload: three-condition OR clause (a0<3 OR a0>17 OR a1='q') where one row
//           matches ONLY via the third condition; verified in memtable, after
//           Flush (SST bitmap union path), and after CompactAll.
// Threat: BuildQueryBitmap's multi-condition union folds the first two
//         condition bitmaps and then accumulates the rest in a loop; a
//         regression there drops rows matched only by the 3rd+ condition.
TEST_F(BitLSMTestBase, ThreeConditionOrClause) {
  BitLSMOptions opt = ThreeAttrOptions();
  CheckedBitLSM db(&OpenDB(opt), opt);
  // k5 matches ONLY via a1='q'; k0-k2 via a0<3; k18-k19 via a0>17.
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(db.Put(
        "k" + std::to_string(i),
        {static_cast<double>(i), std::string(i == 5 ? "q" : "c"), 0.0}, "p"));
  }
  BitLSMQuery q(
      std::vector<OrClause>{{{0, CompareOp::LESS, 3.0},
                             {0, CompareOp::GREATER, 17.0},
                             {1, CompareOp::EQUAL, std::string("q")}}});
  ASSERT_TRUE(db.VerifyQuery(q));  // memtable path
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(q));  // SST bitmap union path
  ASSERT_TRUE(db.CompactAll());
  ASSERT_TRUE(db.VerifyQuery(q));
}
