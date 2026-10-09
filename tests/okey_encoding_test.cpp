#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include "bit_lsm_encoding.h"
#include "bit_lsm_query.h"

using namespace bit_lsm;

// ---- Monotonicity: native order == okey unsigned order ----

TEST(OkeyEncoding, Int64Monotone) {
  const int64_t fixtures[] = {
      std::numeric_limits<int64_t>::min(), -2, -1, 0, 1, 2,
      std::numeric_limits<int64_t>::max()};
  for (int64_t a : fixtures)
    for (int64_t b : fixtures)
      EXPECT_EQ(a < b, I64ToOkey(a) < I64ToOkey(b)) << a << " vs " << b;

  std::mt19937_64 rng(42);
  for (int i = 0; i < 100000; ++i) {
    int64_t a = static_cast<int64_t>(rng()), b = static_cast<int64_t>(rng());
    ASSERT_EQ(a < b, I64ToOkey(a) < I64ToOkey(b));
  }
}

TEST(OkeyEncoding, Uint64Identity) {
  EXPECT_EQ(U64ToOkey(0u), 0u);
  EXPECT_EQ(U64ToOkey(std::numeric_limits<uint64_t>::max()),
            std::numeric_limits<uint64_t>::max());
}

// Includes ±inf, ±denormal, ±huge and zero.
TEST(OkeyEncoding, DoubleMonotone) {
  const double fixtures[] = {-std::numeric_limits<double>::infinity(),
                             -1e300,
                             -1.0,
                             -5e-324 /* -denormal */,
                             0.0,
                             5e-324 /* +denormal */,
                             1.0,
                             1e300,
                             std::numeric_limits<double>::infinity()};
  for (double a : fixtures)
    for (double b : fixtures)
      EXPECT_EQ(a < b, F64ToOkey(a) < F64ToOkey(b)) << a << " vs " << b;

  std::mt19937_64 rng(43);
  std::uniform_real_distribution<double> dist(-1e15, 1e15);
  for (int i = 0; i < 100000; ++i) {
    double a = dist(rng), b = dist(rng);
    ASSERT_EQ(a < b, F64ToOkey(a) < F64ToOkey(b));
  }
}

// -0.0 == +0.0 natively, so they must get the same okey or the bitmap
// phase would prune one on an EQUAL query for the other.
TEST(OkeyEncoding, NegativeZeroCanonicalized) {
  EXPECT_EQ(F64ToOkey(-0.0), F64ToOkey(0.0));
}

// Placement does not matter (row re-verification filters NaN); only a
// crash would.
TEST(OkeyEncoding, NanDoesNotCrash) {
  (void)F64ToOkey(std::numeric_limits<double>::quiet_NaN());
}

// ---- Inverse mapping (debug/Dump only) ----

TEST(OkeyEncoding, RoundTrip) {
  const int64_t ivals[] = {std::numeric_limits<int64_t>::min(), -7, 0, 7,
                           std::numeric_limits<int64_t>::max()};
  for (int64_t v : ivals) EXPECT_EQ(OkeyToI64(I64ToOkey(v)), v);
  const double dvals[] = {-1e300, -1.0, 0.0, 1.0, 1e300};
  for (double v : dvals) EXPECT_EQ(OkeyToF64(F64ToOkey(v)), v);
}

// ---- okey <-> SABI byte domain ----

TEST(OkeyBytes, MemcmpOrderMatchesOkeyOrder) {
  const uint64_t edges[] = {0,
                            1,
                            0x7fffffffffffffffull,
                            0x8000000000000000ull,
                            UINT64_MAX - 1,
                            UINT64_MAX};
  for (uint64_t a : edges)
    for (uint64_t b : edges) ASSERT_EQ(a < b, OkeyToBytes(a) < OkeyToBytes(b));
  std::mt19937_64 rng(7);
  for (int i = 0; i < 100000; ++i) {
    uint64_t a = rng(), b = rng();
    ASSERT_EQ(a < b, OkeyToBytes(a) < OkeyToBytes(b));
  }
}

TEST(OkeyBytes, RoundTrip) {
  const uint64_t vals[] = {0, 1, 0x0123456789abcdefull, 0x8000000000000000ull,
                           UINT64_MAX};
  for (uint64_t v : vals) {
    std::string s = OkeyToBytes(v);
    ASSERT_EQ(s.size(), kOkeyBytes);
    EXPECT_EQ(OkeyFromBytes(s), v);
  }
}

// ---- SABISchema: the schema visible to SABI ----

static BitLSMOptions MakeOpts3() {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8, /*nullable=*/true),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary),
      AttrSpec(IndexType::kRange, PhysicalType::kInt, 4, /*nullable=*/false)};
  o.rho = 0.2;
  return o;
}

TEST(SABISchema, FromOptionsKeepsOnlyRoles) {
  SABISchema s = SABISchema::FromOptions(MakeOpts3());
  ASSERT_EQ(s.attr_num(), 3u);
  EXPECT_EQ(s.index_types[0], IndexType::kRange);
  EXPECT_EQ(s.index_types[1], IndexType::kEquality);
  EXPECT_DOUBLE_EQ(s.rho, 0.2);
}

TEST(EncodeQuery, ComparandsLandInSabiDomain) {
  BitLSMOptions o = MakeOpts3();  // [kRange f64, kEquality, kRange i64]
  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.5},
                                  {1, CompareOp::EQUAL, std::string("seoul")},
                                  {2, CompareOp::LESS, int64_t(-3)}});
  SABIQuery sq = EncodeQuery(q, o);
  ASSERT_EQ(sq.clause_groups.size(), 3u);
  EXPECT_FALSE(sq.unsat);
  EXPECT_EQ(sq.clause_groups[0][0].win.lo, OkeyToBytes(F64ToOkey(10.5)));
  EXPECT_TRUE(sq.clause_groups[0][0].win.hi_unbounded);
  EXPECT_EQ(sq.clause_groups[1][0].bytes, "seoul");
  // Strict upper bound stays on the comparand with the open flag.
  EXPECT_EQ(sq.clause_groups[2][0].win.lo, "");
  EXPECT_EQ(sq.clause_groups[2][0].win.hi, OkeyToBytes(I64ToOkey(-3)));
  EXPECT_TRUE(sq.clause_groups[2][0].win.hi_open);
}

// b >= 10.5 AND b < 20.5 as two clauses must reach bin selection as one
// interval; separately each half-line covers most of the bins.
TEST(EncodeQuery, SameAttrClausesMergeToOneInterval) {
  BitLSMOptions o = MakeOpts3();
  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.5},
                                  {1, CompareOp::EQUAL, std::string("seoul")},
                                  {0, CompareOp::LESS, 20.5}});
  SABIQuery sq = EncodeQuery(q, o);
  ASSERT_EQ(sq.clause_groups.size(), 2u);  // both bounds fold into clause 0
  EXPECT_FALSE(sq.unsat);
  EXPECT_EQ(sq.clause_groups[0][0].attr_idx, 0u);
  EXPECT_EQ(sq.clause_groups[0][0].win.lo, OkeyToBytes(F64ToOkey(10.5)));
  EXPECT_EQ(sq.clause_groups[0][0].win.hi, OkeyToBytes(F64ToOkey(20.5)));
  EXPECT_TRUE(sq.clause_groups[0][0].win.hi_open);
  EXPECT_EQ(sq.clause_groups[1][0].bytes, "seoul");
}

TEST(EncodeQuery, ContradictionSetsUnsat) {
  BitLSMOptions o = MakeOpts3();
  BitLSMQuery q(std::vector<QueryCondition>{{0, CompareOp::GREATER, 20.5},
                                            {0, CompareOp::LESS, 10.5}});
  SABIQuery sq = EncodeQuery(q, o);
  EXPECT_TRUE(sq.unsat);
}

// i64 > INT64_MAX: the bound stays on the comparand with the open flag,
// and every SST prunes it through its max.
TEST(EncodeQuery, StrictBoundAboveDomainMaxStaysOpenOnComparand) {
  BitLSMOptions o = MakeOpts3();
  BitLSMQuery q(std::vector<QueryCondition>{
      {2, CompareOp::GREATER, std::numeric_limits<int64_t>::max()}});
  ASSERT_EQ(I64ToOkey(std::numeric_limits<int64_t>::max()), UINT64_MAX);
  SABIQuery sq = EncodeQuery(q, o);
  EXPECT_FALSE(sq.unsat);
  const ByteInterval& w = sq.clause_groups[0][0].win;
  EXPECT_EQ(w.lo, OkeyToBytes(UINT64_MAX));
  EXPECT_TRUE(w.lo_open);
  EXPECT_TRUE(w.hi_unbounded);
}

// (a=1 OR a=9) is a union, not a window.
TEST(EncodeQuery, OrClauseMembersAreNotMerged) {
  BitLSMOptions o = MakeOpts3();
  BitLSMQuery q(std::vector<OrClause>{
      {{0, CompareOp::EQUAL, 1.0}, {0, CompareOp::EQUAL, 9.0}},
      {{0, CompareOp::LESS_EQUAL, 5.0}}});
  SABIQuery sq = EncodeQuery(q, o);
  ASSERT_EQ(sq.clause_groups.size(), 2u);
  EXPECT_FALSE(sq.unsat);
  ASSERT_EQ(sq.clause_groups[0].size(), 2u);
  EXPECT_EQ(sq.clause_groups[0][0].win.lo, OkeyToBytes(F64ToOkey(1.0)));
  EXPECT_EQ(sq.clause_groups[0][0].win.hi, OkeyToBytes(F64ToOkey(1.0)));
  EXPECT_EQ(sq.clause_groups[0][1].win.lo, OkeyToBytes(F64ToOkey(9.0)));
  ASSERT_EQ(sq.clause_groups[1].size(), 1u);
  EXPECT_EQ(sq.clause_groups[1][0].win.hi, OkeyToBytes(F64ToOkey(5.0)));
}
