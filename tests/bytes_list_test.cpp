#include "bytes_list.h"

#include <gtest/gtest.h>

#include <string>

using namespace bit_lsm;

namespace {
BytesList Sample() {
  BytesList l;
  for (const char* s : {"", "a", "ab", "abc", "b"}) l.push_back(s);
  return l;
}
}  // namespace

// Workload: push five strings including the empty one; index, back, size.
// Threat: cumulative end offsets off by one for the first element, or an
//         empty element read with a stale start offset.
TEST(BytesList, IndexingAndBack) {
  BytesList l = Sample();
  ASSERT_EQ(l.size(), 5u);
  EXPECT_EQ(l[0], "");
  EXPECT_EQ(l[1], "a");
  EXPECT_EQ(l[3], "abc");
  EXPECT_EQ(l.back(), "b");
}

// Workload: UpperBound/LowerBound over the sorted sample for probes that hit
//           an element, fall between elements, sit below all, and sit above
//           all.
// Threat: these are the bin-assignment primitives shared by builder and
//         reader; an off-by-one here mis-bins rows or selects a neighbour bin.
TEST(BytesList, BoundsMatchStdSemantics) {
  BytesList l = Sample();  // "", "a", "ab", "abc", "b"
  EXPECT_EQ(l.UpperBound(""), 1u);
  EXPECT_EQ(l.LowerBound(""), 0u);
  EXPECT_EQ(l.UpperBound("ab"), 3u);
  EXPECT_EQ(l.LowerBound("ab"), 2u);
  EXPECT_EQ(l.UpperBound("aba"), 3u);
  EXPECT_EQ(l.LowerBound("aba"), 3u);
  EXPECT_EQ(l.UpperBound("zzz"), 5u);
  EXPECT_EQ(l.LowerBound("zzz"), 5u);
  EXPECT_EQ(l.LowerBound("b"), 4u);
  EXPECT_EQ(l.UpperBound("b"), 5u);
}

// Workload: serialize the sample, parse it back from the byte buffer, and
//           check the consumed length equals the serialized length.
// Threat: the v8 policy body is exactly this wire form; a length mismatch
//         desynchronizes every following policy in the blob.
TEST(BytesList, SerializeParseRoundTrip) {
  BytesList l = Sample();
  std::string wire;
  l.Serialize(&wire);
  BytesList back;
  size_t consumed = back.Parse(wire.data());
  EXPECT_EQ(consumed, wire.size());
  ASSERT_EQ(back.size(), l.size());
  for (size_t i = 0; i < l.size(); ++i) EXPECT_EQ(back[i], l[i]);
  BytesList empty;
  wire.clear();
  empty.Serialize(&wire);
  EXPECT_EQ(wire.size(), sizeof(uint32_t));
  EXPECT_EQ(back.Parse(wire.data()), sizeof(uint32_t));
  EXPECT_TRUE(back.empty());
}
