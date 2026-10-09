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

// Five strings including the empty one.
TEST(BytesList, IndexingAndBack) {
  BytesList l = Sample();
  ASSERT_EQ(l.size(), 5u);
  EXPECT_EQ(l[0], "");
  EXPECT_EQ(l[1], "a");
  EXPECT_EQ(l[3], "abc");
  EXPECT_EQ(l.back(), "b");
}

// Probes that hit an element, fall between elements, and sit below and
// above all of them.
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

// The consumed length must equal the serialized length, or every
// following policy in the blob is misread.
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
