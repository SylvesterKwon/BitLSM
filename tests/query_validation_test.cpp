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

// Workload: structurally valid queries — full CNF with a cross-attr clause,
//           and the empty query (= full scan).
// Threat: validation rejects queries the contract admits.
TEST(QueryValidation, AcceptsValidFullCnf) {
  BitLSMOptions opt = TwoAttrOptions();
  BitLSMQuery q(std::vector<OrClause>{{{0, CompareOp::GREATER_EQUAL, 1.0},
                                       {1, CompareOp::EQUAL, std::string("x")}},
                                      {{0, CompareOp::LESS, 9.0}}});
  EXPECT_TRUE(q.Validate(opt).ok());
  BitLSMQuery empty;
  EXPECT_TRUE(empty.Validate(opt).ok());
}

// Workload: query containing an empty OR clause.
// Threat: engine UB (clause[0] on an empty vector) instead of clean rejection.
TEST(QueryValidation, RejectsEmptyClause) {
  BitLSMQuery q(std::vector<OrClause>{OrClause{}});
  EXPECT_TRUE(q.Validate(TwoAttrOptions()).IsInvalidArgument());
}

// Workload: condition referencing attr_idx 5 in a 2-attr schema.
// Threat: out-of-bounds attr_specs[]/offset-table access (UB).
TEST(QueryValidation, RejectsAttrIdxOutOfRange) {
  BitLSMQuery q(std::vector<QueryCondition>{{5, CompareOp::EQUAL, 1.0}});
  EXPECT_TRUE(q.Validate(TwoAttrOptions()).IsInvalidArgument());
  BitLSMQuery sentinel(
      std::vector<QueryCondition>{{UINT32_MAX, CompareOp::EQUAL, 1.0}});
  EXPECT_TRUE(sentinel.Validate(TwoAttrOptions()).IsInvalidArgument());
}

// Workload: ordered attr with a string value, unordered with a double.
// Threat: std::get<> on the wrong variant alternative -> bad_variant_access.
TEST(QueryValidation, RejectsValueTypeMismatch) {
  BitLSMQuery q1(
      std::vector<QueryCondition>{{0, CompareOp::EQUAL, std::string("x")}});
  EXPECT_TRUE(q1.Validate(TwoAttrOptions()).IsInvalidArgument());
  BitLSMQuery q2(std::vector<QueryCondition>{{1, CompareOp::EQUAL, 3.0}});
  EXPECT_TRUE(q2.Validate(TwoAttrOptions()).IsInvalidArgument());
}

// Workload: range operator on a unordered attribute.
// Threat: bitmap path asserts (Debug) or returns an empty bitmap (Release,
//         silent row loss) — unordered bins preserve no order, so the
//         contract is EQUAL-only (paper §3: equality predicates).
TEST(QueryValidation, RejectsNonEqualOnUnordered) {
  BitLSMQuery q(std::vector<QueryCondition>{
      {1, CompareOp::GREATER_EQUAL, std::string("x")}});
  EXPECT_TRUE(q.Validate(TwoAttrOptions()).IsInvalidArgument());
}

// Workload: NewIterator with an invalid query, then with a valid one.
// Threat: invalid query reaches the iterator (UB) instead of returning null.
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

// Workload: the operator/comparand matrix over one attr of each physical
//           type and both index types.
// Threat: Validate is the only gate between a query and the typed re-check;
//         a wrong alternative reaches std::get (throws) and a range operator
//         on a kEquality attr reaches SelectBins (asserts).
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
