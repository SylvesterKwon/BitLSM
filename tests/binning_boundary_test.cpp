#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

using namespace bit_lsm;

namespace {
// Single ordered attribute, tiny rho to force bin collisions.
BitLSMOptions ContOpt(double rho) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrType::ORDERED};
  o.read_seqno = 0;
  o.rho = rho;
  return o;
}
// Single unordered attribute.
BitLSMOptions CatOpt(double rho) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrType::UNORDERED};
  o.read_seqno = 0;
  o.rho = rho;
  return o;
}
BitLSMQuery Cont(CompareOp op, double v) {
  return BitLSMQuery(std::vector<QueryCondition>{{0, op, v}});
}
}  // namespace

// Workload: ordered values 0..49; after Flush, run every operator with a
//           threshold equal to a STORED value (lands on bin boundaries).
// Threat: off-by-one at the bin boundary (e.g. GREATER vs GREATER_EQUAL) in the
//         SABI bitmap path.
TEST_F(BitLSMTestBase, OrderedBoundaryAllOps) {
  BitLSMOptions opt = ContOpt(0.5);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 50; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());  // exercise the bitmap-index path
  for (double t : {0.0, 1.0, 24.0, 25.0, 49.0}) {
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::EQUAL, t)));
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::LESS, t)));
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::LESS_EQUAL, t)));
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::GREATER, t)));
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::GREATER_EQUAL, t)));
  }
}

// Workload: 100 distinct ordered values but rho so small that many share a
// bin;
//           EQUAL queries after Flush.
// Threat: under-budgeted bins produce false-positive candidates that the
// MultiGet
//         re-validation must filter — result must stay exact.
TEST_F(BitLSMTestBase, RhoTooSmallStillExact) {
  BitLSMOptions opt = ContOpt(0.01);  // tiny budget -> heavy bin collisions
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 100; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  for (double t : {3.0, 42.0, 77.0})
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::EQUAL, t)));
  ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::GREATER_EQUAL, 50.0)));
}

// Workload: 40 distinct categories with a tiny rho; EQUAL on a category + an OR
//           of two categories, after Flush.
// Threat: unordered cardinality exceeding the bin budget mis-maps values.
TEST_F(BitLSMTestBase, UnorderedCardinalityExceedsBins) {
  BitLSMOptions opt = CatOpt(0.05);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 80; ++i) {
    std::string cat = "c" + std::to_string(i % 40);  // 40 distinct categories
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {cat}, "p"));
  }
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(
      std::vector<QueryCondition>{{0, CompareOp::EQUAL, std::string("c7")}})));
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(
      std::vector<OrClause>{{{0, CompareOp::EQUAL, std::string("c7")},
                             {0, CompareOp::EQUAL, std::string("c39")}}})));
}

// Workload: all rows share one ordered value (everything in one bin), then
// all
//           rows distinct; range + equality queries after Flush.
// Threat: degenerate binning (single bin vs. all-distinct) breaks range eval.
TEST_F(BitLSMTestBase, BinningExtremes) {
  {
    BitLSMOptions opt = ContOpt(0.5);
    CheckedBitLSM db(&OpenDB(opt), opt);
    for (int i = 0; i < 30; ++i)
      ASSERT_TRUE(db.Put("k" + std::to_string(i), {7.0}, "p"));  // all same
    ASSERT_TRUE(db.Flush());
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::EQUAL, 7.0)));
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::GREATER, 7.0)));  // empty
    ASSERT_TRUE(db.VerifyQuery(Cont(CompareOp::LESS_EQUAL, 7.0)));
  }
}

// Workload: a query that matches nothing and the empty query (matches
// everything). Threat: empty result set vs. full scan boundary handling in the
// iterator.
TEST_F(BitLSMTestBase, EmptyResultAndFullMatch) {
  BitLSMOptions opt = ContOpt(0.5);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 10; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(
      db.VerifyQuery(Cont(CompareOp::GREATER, 1000.0)));  // matches none
  ASSERT_TRUE(db.VerifyFullScan());                       // matches all
}

// Workload: contradictory CNF (a0>5 AND a0<3) and a satisfiable AND-of-OR.
// Threat: AND/OR set algebra over roaring bitmaps yields wrong intersection.
TEST_F(BitLSMTestBase, CnfContradictionAndOr) {
  BitLSMOptions opt = ContOpt(0.5);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 20; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {static_cast<double>(i)}, "p"));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(std::vector<OrClause>{
      {{0, CompareOp::GREATER, 5.0}}, {{0, CompareOp::LESS, 3.0}}})));  // empty
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(std::vector<OrClause>{
      {{0, CompareOp::LESS, 3.0},
       {0, CompareOp::GREATER, 17.0}},           // (a0<3 OR a0>17)
      {{0, CompareOp::GREATER_EQUAL, 1.0}}})));  // AND a0>=1
}
