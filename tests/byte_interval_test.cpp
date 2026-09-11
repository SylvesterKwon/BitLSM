#include <gtest/gtest.h>

#include <string>

#include "bit_lsm_query.h"

using namespace bit_lsm;

// Workload: every CompareOp against the comparand "abc" through FromOp.
// Threat: a strict bound canonicalized on the wrong side ('\0' successor on
//         hi, or an open flag on lo) shifts the bin extent by one bin.
TEST(ByteInterval, FromOpShapes) {
  ByteInterval eq = ByteInterval::FromOp(CompareOp::EQUAL, "abc");
  EXPECT_EQ(eq.lo, "abc");
  EXPECT_EQ(eq.hi, "abc");
  EXPECT_FALSE(eq.hi_open);
  EXPECT_FALSE(eq.hi_unbounded);

  ByteInterval ge = ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "abc");
  EXPECT_EQ(ge.lo, "abc");
  EXPECT_TRUE(ge.hi_unbounded);

  ByteInterval gt = ByteInterval::FromOp(CompareOp::GREATER, "abc");
  EXPECT_EQ(gt.lo, "abc");
  EXPECT_TRUE(gt.lo_open);
  EXPECT_TRUE(gt.hi_unbounded);
  EXPECT_FALSE(ge.lo_open);

  ByteInterval le = ByteInterval::FromOp(CompareOp::LESS_EQUAL, "abc");
  EXPECT_EQ(le.lo, "");
  EXPECT_EQ(le.hi, "abc");
  EXPECT_FALSE(le.hi_open);
  EXPECT_FALSE(le.hi_unbounded);

  ByteInterval lt = ByteInterval::FromOp(CompareOp::LESS, "abc");
  EXPECT_EQ(lt.hi, "abc");
  EXPECT_TRUE(lt.hi_open);
  EXPECT_FALSE(lt.hi_unbounded);
}

// Workload: x < "" and x <= "" (the domain minimum), x > "" and x >= "".
// Threat: "" is the least byte string; an interval below it must be empty
//         and one at or above it must not be.
TEST(ByteInterval, DomainMinimumEdges) {
  EXPECT_TRUE(ByteInterval::FromOp(CompareOp::LESS, "").Empty());
  EXPECT_FALSE(ByteInterval::FromOp(CompareOp::LESS_EQUAL, "").Empty());
  EXPECT_FALSE(ByteInterval::FromOp(CompareOp::GREATER, "").Empty());
  EXPECT_FALSE(ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "").Empty());
}

// Workload: BETWEEN-shaped pairs intersected: [b, inf) with (-inf, d);
//           [b, inf) with (-inf, b); [b, inf) with (-inf, b]; two upper bounds
//           that differ only in openness.
// Threat: Intersect that keeps the looser upper bound, or drops the open
//         flag when the tighter bound is open, over-selects bins.
TEST(ByteInterval, IntersectKeepsTighterBound) {
  ByteInterval w = ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "b");
  w.Intersect(ByteInterval::FromOp(CompareOp::LESS, "d"));
  EXPECT_EQ(w.lo, "b");
  EXPECT_EQ(w.hi, "d");
  EXPECT_TRUE(w.hi_open);
  EXPECT_FALSE(w.Empty());

  ByteInterval e = ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "b");
  e.Intersect(ByteInterval::FromOp(CompareOp::LESS, "b"));
  EXPECT_TRUE(e.Empty());

  ByteInterval p = ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "b");
  p.Intersect(ByteInterval::FromOp(CompareOp::LESS_EQUAL, "b"));
  EXPECT_FALSE(p.Empty());
  EXPECT_EQ(p.lo, "b");
  EXPECT_EQ(p.hi, "b");

  ByteInterval o = ByteInterval::FromOp(CompareOp::LESS_EQUAL, "c");
  o.Intersect(ByteInterval::FromOp(CompareOp::LESS, "c"));
  EXPECT_TRUE(o.hi_open);
  ByteInterval o2 = ByteInterval::FromOp(CompareOp::LESS, "c");
  o2.Intersect(ByteInterval::FromOp(CompareOp::LESS_EQUAL, "c"));
  EXPECT_TRUE(o2.hi_open);
}

// Workload: strict lower bounds meeting closed ones on the same comparand:
//           x > "b" with x >= "b" (either order), x > "b" with x <= "b".
// Threat: a tie on lo that drops the open flag turns x > b into x >= b; a
//         point closed on one side and open on the other is empty.
TEST(ByteInterval, StrictLowerBoundTies) {
  ByteInterval a = ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "b");
  a.Intersect(ByteInterval::FromOp(CompareOp::GREATER, "b"));
  EXPECT_TRUE(a.lo_open);
  ByteInterval b = ByteInterval::FromOp(CompareOp::GREATER, "b");
  b.Intersect(ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "b"));
  EXPECT_TRUE(b.lo_open);
  ByteInterval c = ByteInterval::FromOp(CompareOp::GREATER, "b");
  c.Intersect(ByteInterval::FromOp(CompareOp::LESS_EQUAL, "b"));
  EXPECT_TRUE(c.Empty());
  ByteInterval d = ByteInterval::FromOp(CompareOp::GREATER, "b");
  d.Intersect(ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "c"));
  EXPECT_EQ(d.lo, "c");
  EXPECT_FALSE(d.lo_open);
}

// Workload: contradictory bounds x > "z" AND x < "a"; and an empty interval
//           intersected with an unbounded one.
// Threat: Empty must be absorbing -- a later Intersect with a wide interval
//         must not resurrect a contradiction.
TEST(ByteInterval, EmptyIsAbsorbing) {
  ByteInterval w = ByteInterval::FromOp(CompareOp::GREATER, "z");
  w.Intersect(ByteInterval::FromOp(CompareOp::LESS, "a"));
  EXPECT_TRUE(w.Empty());
  w.Intersect(ByteInterval::FromOp(CompareOp::GREATER_EQUAL, ""));
  EXPECT_TRUE(w.Empty());
}
