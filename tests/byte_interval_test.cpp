#include <gtest/gtest.h>

#include <string>

#include "bit_lsm_query.h"

using namespace bit_lsm;

// Every CompareOp against "abc".
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

// "" is the least byte string: an interval below it is empty, one at or
// above it is not.
TEST(ByteInterval, DomainMinimumEdges) {
  EXPECT_TRUE(ByteInterval::FromOp(CompareOp::LESS, "").Empty());
  EXPECT_FALSE(ByteInterval::FromOp(CompareOp::LESS_EQUAL, "").Empty());
  EXPECT_FALSE(ByteInterval::FromOp(CompareOp::GREATER, "").Empty());
  EXPECT_FALSE(ByteInterval::FromOp(CompareOp::GREATER_EQUAL, "").Empty());
}

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

// x > "b" with x >= "b" must keep the open flag; x > "b" with x <= "b"
// is empty.
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

TEST(ByteInterval, EmptyIsAbsorbing) {
  ByteInterval w = ByteInterval::FromOp(CompareOp::GREATER, "z");
  w.Intersect(ByteInterval::FromOp(CompareOp::LESS, "a"));
  EXPECT_TRUE(w.Empty());
  w.Intersect(ByteInterval::FromOp(CompareOp::GREATER_EQUAL, ""));
  EXPECT_TRUE(w.Empty());
}
