#include "test_util/reference_db.h"
#include <gtest/gtest.h>

using namespace bit_lsm;

namespace {
BitLSMOptions Opt2() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_types = {AttrType::CONTINUOUS, AttrType::CATEGORICAL};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}
}  // namespace

// Workload: evaluate Match on continuous conditions with all five operators.
// Threat: a wrong oracle silently invalidates EVERY differential test built on it.
TEST(ReferenceDBTest, MatchContinuousOps) {
  ReferenceDB ref(Opt2());
  std::vector<Attr> attrs{10.0, std::string("x")};
  EXPECT_TRUE(ref.Match(BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}}), attrs));
  EXPECT_FALSE(ref.Match(BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::GREATER, 10.0}}), attrs));
  EXPECT_TRUE(ref.Match(BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::LESS_EQUAL, 10.0}}), attrs));
  EXPECT_FALSE(ref.Match(BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::LESS, 10.0}}), attrs));
  EXPECT_TRUE(ref.Match(BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::EQUAL, 10.0}}), attrs));
}

// Workload: CNF with an OR-clause and an AND-clause; a contradiction; empty query.
// Threat: AND/OR nesting wrong → oracle disagrees with engine for the wrong reason.
TEST(ReferenceDBTest, MatchCnfAndOr) {
  ReferenceDB ref(Opt2());
  std::vector<Attr> attrs{10.0, std::string("x")};
  BitLSMQuery q(std::vector<OrClause>{
      {{0, CompareOp::EQUAL, 10.0}, {0, CompareOp::EQUAL, 20.0}},  // (a0=10 OR a0=20)
      {{1, CompareOp::EQUAL, std::string("x")}}});                 // AND (a1='x')
  EXPECT_TRUE(ref.Match(q, attrs));

  BitLSMQuery contra(std::vector<OrClause>{
      {{0, CompareOp::GREATER, 5.0}}, {{0, CompareOp::LESS, 3.0}}});  // a0>5 AND a0<3
  EXPECT_FALSE(ref.Match(contra, attrs));

  EXPECT_TRUE(ref.Match(BitLSMQuery(), attrs));  // empty matches all
}

// Workload: Put/overwrite/Delete, then ExpectedResult over the empty query.
// Threat: overwrite must be latest-wins and Delete must remove the key.
TEST(ReferenceDBTest, PutOverwriteDeleteExpectedResult) {
  ReferenceDB ref(Opt2());
  ref.Put("k1", {10.0, std::string("a")}, "p1");
  ref.Put("k2", {20.0, std::string("b")}, "p2");
  ref.Put("k1", {30.0, std::string("a")}, "p1b");  // overwrite k1
  ref.Delete("k2");

  auto r = ref.ExpectedResult(BitLSMQuery());  // all live
  ASSERT_EQ(r.size(), 1u);
  ASSERT_TRUE(r.count("k1"));
  EXPECT_EQ(std::get<double>(r.at("k1").attrs[0]), 30.0);
  EXPECT_EQ(r.at("k1").payload, "p1b");
}
