#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bit_lsm_query.h"
#include "test_util/bitlsm_test_base.h"

using namespace bit_lsm;

namespace {
BitLSMOptions TwoAttrOptions() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                  AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}
}  // namespace

TEST(QueryValidation, AcceptsValidFullCnf) {
  BitLSMOptions opt = TwoAttrOptions();
  BitLSMQuery q(std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 1.0},
                                       {1, CompareOp::EQUAL, std::string("x")}},
                                      {{0, CompareOp::LESS, 9.0}}});
  EXPECT_TRUE(q.Validate(opt).ok());
  BitLSMQuery empty;
  EXPECT_TRUE(empty.Validate(opt).ok());
}

TEST(QueryValidation, RejectsEmptyClause) {
  BitLSMQuery q(std::vector<OrClause>{OrClause{}});
  EXPECT_TRUE(q.Validate(TwoAttrOptions()).IsInvalidArgument());
}

TEST(QueryValidation, RejectsAttrIdxOutOfRange) {
  BitLSMQuery q(std::vector<QueryCondition>{{5, CompareOp::EQUAL, 1.0}});
  EXPECT_TRUE(q.Validate(TwoAttrOptions()).IsInvalidArgument());
  BitLSMQuery sentinel(
      std::vector<QueryCondition>{{UINT32_MAX, CompareOp::EQUAL, 1.0}});
  EXPECT_TRUE(sentinel.Validate(TwoAttrOptions()).IsInvalidArgument());
}

TEST(QueryValidation, RejectsValueTypeMismatch) {
  BitLSMQuery q1(
      std::vector<QueryCondition>{{0, CompareOp::EQUAL, std::string("x")}});
  EXPECT_TRUE(q1.Validate(TwoAttrOptions()).IsInvalidArgument());
  BitLSMQuery q2(std::vector<QueryCondition>{{1, CompareOp::EQUAL, 3.0}});
  EXPECT_TRUE(q2.Validate(TwoAttrOptions()).IsInvalidArgument());
}

// Unordered bins preserve no order, so only EQUAL is allowed.
TEST(QueryValidation, RejectsNonEqualOnUnordered) {
  BitLSMQuery q(std::vector<QueryCondition>{
      {1, CompareOp::GREATER_EQUAL, std::string("x")}});
  EXPECT_TRUE(q.Validate(TwoAttrOptions()).IsInvalidArgument());
}

TEST_F(BitLSMTestBase, NewIteratorRejectsInvalidQuery) {
  BitLSMOptions opt = DefaultOptions();
  BitLSM& db = OpenDB(opt);
  ASSERT_TRUE(db.Put("k1", {1.0, std::string("a")}, "p").ok());
  BitLSMQuery bad(std::vector<OrClause>{OrClause{}});
  EXPECT_EQ(db.NewIterator(bad), nullptr);
  BitLSMQuery good(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 0.0}});
  EXPECT_NE(db.NewIterator(good), nullptr);
}

// Validate is the only gate before the typed re-check and SelectBins.
TEST(QueryValidation, PhysicalTypeAndIndexTypeMatrix) {
  BitLSMOptions o;
  o.attr_num = 5;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kInt, 4),
                  AttrSpec(IndexType::kRange, PhysicalType::kUint, 8),
                  AttrSpec(IndexType::kEquality, PhysicalType::kFloat, 8),
                  AttrSpec(IndexType::kRange, PhysicalType::kBinary, 4),
                  AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  auto ok = [&](uint32_t a, CompareOp op, QueryCondition::value_type v) {
    return BitLSMQuery(std::vector<QueryCondition>{{a, op, std::move(v)}})
        .Validate(o)
        .ok();
  };
  EXPECT_TRUE(ok(0, CompareOp::LESS, int64_t(3)));
  EXPECT_FALSE(ok(0, CompareOp::LESS, 3.0));
  EXPECT_TRUE(ok(1, CompareOp::GREATER_EQUAL, uint64_t(3)));
  EXPECT_FALSE(ok(1, CompareOp::GREATER_EQUAL, int64_t(3)));
  EXPECT_TRUE(ok(2, CompareOp::EQUAL, 1.5));
  EXPECT_FALSE(ok(2, CompareOp::LESS, 1.5));  // kEquality: EQUAL only
  EXPECT_TRUE(ok(3, CompareOp::GREATER, std::string("ab")));
  EXPECT_FALSE(ok(3, CompareOp::GREATER, int64_t(1)));
  EXPECT_TRUE(ok(4, CompareOp::EQUAL, std::string("x")));
  EXPECT_FALSE(ok(4, CompareOp::LESS_EQUAL, std::string("x")));
}
