#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include "bit_lsm_encoding.h"
#include "bit_lsm_query.h"

using namespace bit_lsm;

// ---- Monotonicity: native order == okey unsigned order ----

// Workload: int64 extremes/near-zero fixtures plus 100k random pairs, compared
//           in native and okey domains.
// Threat: a non-monotone I64ToOkey (e.g. missing sign-bit flip) reorders
//         negative vs positive values, corrupting every SABI bin decision.
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

// Workload: uint64 domain endpoints through U64ToOkey.
// Threat: any transformation of the already-ordered uint64 domain would break
//         the identity contract other encoders are calibrated against.
TEST(OkeyEncoding, Uint64Identity) {
  EXPECT_EQ(U64ToOkey(0u), 0u);
  EXPECT_EQ(U64ToOkey(std::numeric_limits<uint64_t>::max()),
            std::numeric_limits<uint64_t>::max());
}

// Workload: double fixtures spanning ±inf, ±denormal, ±huge, zero plus 100k
//           random pairs, compared in native and okey domains.
// Threat: mishandled IEEE sign-magnitude layout (negatives need full bit flip)
//         inverts the order of negative doubles in okey space.
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

// Workload: -0.0 and +0.0 through F64ToOkey.
// Threat: native == treats them as equal, so distinct okeys would let the
//         bitmap phase prune a -0.0 row on an EQ +0.0 query (false negative).
TEST(OkeyEncoding, NegativeZeroCanonicalized) {
  EXPECT_EQ(F64ToOkey(-0.0), F64ToOkey(0.0));
}

// Workload: quiet NaN through F64ToOkey.
// Threat: placement is correctness-neutral (row re-verification filters NaN);
//         only a crash/UB on the encode path would matter.
TEST(OkeyEncoding, NanDoesNotCrash) {
  (void)F64ToOkey(std::numeric_limits<double>::quiet_NaN());
}

// ---- Inverse mapping (debug/Dump only) ----

// Workload: representative int64/double values through okey and back.
// Threat: a lossy round trip would make Dump/debug output misreport stored
//         values and mask encoder bugs.
TEST(OkeyEncoding, RoundTrip) {
  const int64_t ivals[] = {std::numeric_limits<int64_t>::min(), -7, 0, 7,
                           std::numeric_limits<int64_t>::max()};
  for (int64_t v : ivals) EXPECT_EQ(OkeyToI64(I64ToOkey(v)), v);
  const double dvals[] = {-1e300, -1.0, 0.0, 1.0, 1e300};
  for (double v : dvals) EXPECT_EQ(OkeyToF64(F64ToOkey(v)), v);
}

// ---- t-digest boundary conversion: clamped + monotone ----

// Workload: t-digest quantile estimates below/inside/above the shifted span
//           mapped back to absolute okey thresholds.
// Threat: unclamped negatives/NaN/overflow would wrap around in the uint64
//         cast and produce wildly wrong bin boundaries.
TEST(OkeyEncoding, TDigestBoundaryClamps) {
  EXPECT_EQ(TDigestBoundaryToOkey(-1.0, 0), 0u);
  EXPECT_EQ(TDigestBoundaryToOkey(std::nan(""), 0), 0u);
  EXPECT_EQ(TDigestBoundaryToOkey(1.8446744073709552e19, 0),  // >= 2^64
            std::numeric_limits<uint64_t>::max());
  EXPECT_LE(TDigestBoundaryToOkey(100.5, 0), TDigestBoundaryToOkey(200.5, 0));

  // Sub-min estimates and headroom overflow clamp to the shifted domain.
  uint64_t min_okey = I64ToOkey(0);  // 2^63
  EXPECT_EQ(TDigestBoundaryToOkey(-1.0, min_okey), min_okey);
  EXPECT_EQ(TDigestBoundaryToOkey(std::nan(""), min_okey), min_okey);
  EXPECT_EQ(TDigestBoundaryToOkey(9.3e18, min_okey),  // > 2^64 - min_okey
            std::numeric_limits<uint64_t>::max());
}

// Workload: a narrow okey span near 2^63 (small signed ints, where the double
//           ULP is 2048) through the shifted projection and back.
// Threat: projecting absolute okeys to double collapses any span narrower
//         than the local ULP onto one value, folding every row into a single
//         bin; the min-shift must keep such spans exactly representable.
TEST(OkeyEncoding, TDigestShiftPreservesNarrowSpans) {
  uint64_t min_okey = I64ToOkey(0);  // 2^63
  // Unshifted, all of 0..999 lands on the same double.
  EXPECT_EQ(static_cast<double>(I64ToOkey(0)),
            static_cast<double>(I64ToOkey(999)));
  // Shifted, every okey in the span survives the round trip exactly.
  for (int64_t v : {0, 1, 2, 500, 998, 999}) {
    uint64_t okey = I64ToOkey(v);
    EXPECT_EQ(TDigestBoundaryToOkey(OkeyToTDigest(okey, min_okey), min_okey),
              okey)
        << "v=" << v;
  }
}

// ---- SABISchema: the schema residue visible to SABI ----

static BitLSMOptions MakeOpts3() {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, true, true),
                  AttrSpec(AttrRole::UNORDERED),
                  AttrSpec(AttrRole::ORDERED, 4, true, false, false)};
  o.rho = 0.2;
  return o;
}

// Workload: derive SABISchema from a 3-attr BitLSMOptions.
// Threat: dropping or reordering roles during derivation would make SABI
//         parse bins with the wrong binning-policy variant.
TEST(SABISchema, FromOptionsKeepsOnlyRoles) {
  SABISchema s = SABISchema::FromOptions(MakeOpts3());
  ASSERT_EQ(s.attr_num(), 3u);
  EXPECT_EQ(s.roles[0], AttrRole::ORDERED);
  EXPECT_EQ(s.roles[1], AttrRole::UNORDERED);
  EXPECT_DOUBLE_EQ(s.rho, 0.2);
}

// Workload: a 3-clause query over [ORDERED f64, UNORDERED, ORDERED i64]
//           through the standalone adapter's EncodeQuery.
// Threat: a comparand encoded with the wrong AttrSpec (or left native) makes
//         every downstream bin comparison meaningless.
TEST(EncodeQuery, ComparandsLandInSabiDomain) {
  BitLSMOptions o = MakeOpts3();  // [ORDERED f64, UNORDERED, ORDERED i64]
  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.5},
                                  {1, CompareOp::EQUAL, std::string("seoul")},
                                  {2, CompareOp::LESS, int64_t(-3)}});
  SABIQuery sq = EncodeQuery(q, o);
  ASSERT_EQ(sq.clause_groups.size(), 3u);
  EXPECT_EQ(sq.clause_groups[0][0].okey, F64ToOkey(10.5));
  EXPECT_EQ(sq.clause_groups[1][0].bytes, "seoul");
  EXPECT_EQ(sq.clause_groups[2][0].okey, I64ToOkey(-3));
  EXPECT_EQ(sq.clause_groups[2][0].op, CompareOp::LESS);
}
