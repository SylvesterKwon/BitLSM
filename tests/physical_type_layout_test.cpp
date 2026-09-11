#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

using namespace bit_lsm;

// Workload: kEquality kInt 8 (ids), kRange kBinary 4 (fixed codes),
//           kRange kUint 2; 200 rows; EQUAL on the id, all operators on the
//           4-byte code and the uint16, flushed.
// Threat: a kEquality numeric must reach SABI as canonical okey bytes (so
//         EQUAL hits the dictionary), a kBinary attr must be located at its
//         fixed slot by the compiled re-check, and narrow widths must
//         sign/zero-extend exactly as before.
TEST_F(BitLSMTestBase, MixedPhysicalTypesEndToEnd) {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {AttrSpec(IndexType::kEquality, PhysicalType::kInt, 8),
                  AttrSpec(IndexType::kRange, PhysicalType::kBinary, 4),
                  AttrSpec(IndexType::kRange, PhysicalType::kUint, 2)};
  o.read_seqno = 0;
  o.rho = 0.2;
  CheckedBitLSM db(&OpenDB(o), o);
  for (int i = 0; i < 200; ++i) {
    char code[8];
    std::snprintf(code, sizeof code, "C%03d", i % 50);
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       {int64_t(i % 7), std::string(code), uint64_t(i * 3)},
                       "p"));
  }
  ASSERT_TRUE(db.Flush());
  auto one = [](uint32_t a, CompareOp op, QueryCondition::value_type v) {
    return BitLSMQuery(std::vector<QueryCondition>{{a, op, std::move(v)}});
  };
  for (int64_t id : {0, 3, 6, 9})
    ASSERT_TRUE(db.VerifyQuery(one(0, CompareOp::EQUAL, id)));
  for (const char* c : {"C000", "C025", "C0", "C049", "D"})
    for (CompareOp op :
         {CompareOp::EQUAL, CompareOp::LESS, CompareOp::GREATER_EQUAL})
      ASSERT_TRUE(db.VerifyQuery(one(1, op, std::string(c))));
  for (uint64_t u : {0, 300, 597, 1000})
    for (CompareOp op : {CompareOp::LESS_EQUAL, CompareOp::GREATER})
      ASSERT_TRUE(db.VerifyQuery(one(2, op, u)));
  BitLSMQuery conj(std::vector<OrClause>{
      {{0, CompareOp::EQUAL, int64_t(2)}},
      {{1, CompareOp::GREATER_EQUAL, std::string("C020")}},
      {{2, CompareOp::LESS, uint64_t(400)}}});
  ASSERT_TRUE(db.VerifyQuery(conj));
}

// Workload: Put a 3-byte and a 5-byte value into a kBinary(4) attr, and
//           schemas with kInt width 3 and kVarBinary width 8.
// Threat: silently truncating or over-reading a fixed slot corrupts the
//         neighbouring slot; all must be refused with InvalidArgument.
TEST_F(BitLSMTestBase, FixedBinaryWidthIsEnforced) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kBinary, 4)};
  o.read_seqno = 0;
  o.rho = 0.5;
  BitLSM& db = OpenDB(o);
  EXPECT_TRUE(db.Put("k1", {std::string("abcd")}, "p").ok());
  EXPECT_TRUE(db.Put("k2", {std::string("abc")}, "p").IsInvalidArgument());
  EXPECT_TRUE(db.Put("k3", {std::string("abcde")}, "p").IsInvalidArgument());

  BitLSMOptions bad;
  bad.attr_num = 1;
  bad.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kInt, 3)};
  EXPECT_TRUE(ValidateAttrSpecs(bad).IsInvalidArgument());
  BitLSMOptions bad_var;
  bad_var.attr_num = 1;
  bad_var.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kVarBinary, 8)};
  EXPECT_TRUE(ValidateAttrSpecs(bad_var).IsInvalidArgument());
}
