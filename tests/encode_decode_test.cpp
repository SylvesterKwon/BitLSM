#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"

using namespace bit_lsm;

namespace {
BitLSMOptions MakeOptions(std::vector<AttrSpec> types) {
  BitLSMOptions options;
  options.attr_num = static_cast<uint32_t>(types.size());
  options.attr_specs = std::move(types);
  options.read_seqno = 0;
  options.rho = 0.5;
  return options;
}
}  // namespace

// 연속형 + 범주형 혼합 값이 왕복(encode→decode)에서 보존되는지.
TEST(EncodeDecode, RoundTripMixedAttrs) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED, AttrRole::UNORDERED});
  std::string out;
  EncodeValue(options, {3.14, std::string("apple")}, "payload", out);

  ValueLayout layout(options);
  std::string_view buf(out);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 0)), 3.14);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 1)), "apple");
  EXPECT_EQ(DecodePayload(layout, buf), "payload");
}

// 가변 길이 범주형 두 개(마지막이 아닌 범주형의 길이 계산)와 빈 payload.
TEST(EncodeDecode, RoundTripMultipleUnorderedEmptyPayload) {
  BitLSMOptions options = MakeOptions(
      {AttrRole::UNORDERED, AttrRole::UNORDERED, AttrRole::ORDERED});
  std::string out;
  EncodeValue(options, {std::string("ab"), std::string("cdef"), 2.5}, "", out);

  ValueLayout layout(options);
  std::string_view buf(out);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 0)), "ab");
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 1)), "cdef");
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 2)), 2.5);
  EXPECT_EQ(DecodePayload(layout, buf), "");
}

// Workload: all-ordered schema (n_unordered == 0) with a payload.
// Threat: the v2 header is zero bytes in this case — payload start must be
//         derived purely from the schema (8B x n_cont), and an off-by-one
//         there corrupts every attribute and the payload.
TEST(EncodeDecode, RoundTripAllOrderedZeroHeader) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED, AttrRole::ORDERED});
  std::string out;
  EncodeValue(options, {1.5, -2.5}, "tail", out);

  ValueLayout layout(options);
  EXPECT_EQ(layout.unordered_base, 2 * sizeof(double));
  EXPECT_EQ(out.size(), 2 * sizeof(double) + 4);

  std::string_view buf(out);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 0)), 1.5);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 1)), -2.5);
  EXPECT_EQ(DecodePayload(layout, buf), "tail");
}

// Workload: a double-only schema must serialize byte-identically to v2 (its
// value encoding is unchanged), so DBs/experiments reproduce.
// Threat: v3's width-based layout drifts from v2's hardcoded 8B for doubles.
TEST(EncodeDecode, DoubleSchemaByteIdenticalToV2) {
  BitLSMOptions options = MakeOptions({AttrRole::ORDERED, AttrRole::UNORDERED});
  std::string out;
  EncodeValue(options, {3.14, std::string("apple")}, "pay", out);
  // v2 layout for {double, string}: [var_end u32][double 8B][cat
  // bytes][payload]
  ASSERT_EQ(out.size(), sizeof(uint32_t) + sizeof(double) + 5 + 3);
  uint32_t var_end;
  std::memcpy(&var_end, out.data(), sizeof(uint32_t));
  EXPECT_EQ(var_end, 5u);  // "apple"
  double d;
  std::memcpy(&d, out.data() + sizeof(uint32_t), sizeof(double));
  EXPECT_DOUBLE_EQ(d, 3.14);
}

// Workload: native fixed-width ORDERED types (int32/int64/uint32) round-trip
// through the width-based slots, including negative sign-extension and a
// narrow slot that shrinks the row.
// Threat: wrong slot width, missing sign-extension, or endian mishandling.
TEST(EncodeDecode, RoundTripNativeIntegers) {
  BitLSMOptions options;
  options.attr_num = 3;
  options.read_seqno = 0;
  options.rho = 0.5;
  options.attr_specs = {
      AttrSpec(AttrRole::ORDERED, 4, /*signed=*/true, /*is_float=*/false),
      AttrSpec(AttrRole::ORDERED, 8, /*signed=*/true, /*is_float=*/false),
      AttrSpec(AttrRole::ORDERED, 4, /*signed=*/false, /*is_float=*/false)};

  ValueLayout layout(options);
  // 4B + 8B + 4B fixed region, no unordered, no payload.
  EXPECT_EQ(layout.unordered_base, 16u);

  std::string out;
  EncodeValue(layout,
              {int64_t{-5}, int64_t{-9000000000LL}, uint64_t{4000000000ULL}},
              "", out);
  EXPECT_EQ(out.size(), 16u);

  std::string_view buf(out);
  EXPECT_EQ(std::get<int64_t>(DecodeAttr(layout, buf, 0)), -5);  // int32 -5
  EXPECT_EQ(std::get<int64_t>(DecodeAttr(layout, buf, 1)),
            -9000000000LL);  // int64 beyond int32
  EXPECT_EQ(std::get<uint64_t>(DecodeAttr(layout, buf, 2)),
            4000000000ULL);  // uint32 > INT32_MAX
}

// Workload: nullable ORDERED + nullable UNORDERED; a NULL in each position
// round-trips as monostate while the sibling non-null value survives, and a
// NULL unordered is distinguished from an empty-string unordered.
// Threat: null-bitmap offset math, or a NULL reading back as 0 / "".
TEST(EncodeDecode, RoundTripNullAttrs) {
  BitLSMOptions options;
  options.attr_num = 2;
  options.read_seqno = 0;
  options.rho = 0.5;
  options.attr_specs = {
      AttrSpec(AttrRole::ORDERED, 8, true, true, /*nullable=*/true),
      AttrSpec(AttrRole::UNORDERED, 8, true, true, /*nullable=*/true)};

  ValueLayout layout(options);
  EXPECT_EQ(layout.null_bitmap_bytes, 1u);  // 2 nullable attrs -> 1 byte

  // ordered NULL, unordered "x"
  std::string a;
  EncodeValue(layout, {std::monostate{}, std::string("x")}, "pa", a);
  std::string_view va(a);
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(DecodeAttr(layout, va, 0)));
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, va, 1)), "x");
  EXPECT_EQ(DecodePayload(layout, va), "pa");

  // ordered 5.0, unordered NULL
  std::string b;
  EncodeValue(layout, {5.0, std::monostate{}}, "pb", b);
  std::string_view vb(b);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, vb, 0)), 5.0);
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(DecodeAttr(layout, vb, 1)));
  EXPECT_EQ(DecodePayload(layout, vb), "pb");

  // unordered empty-string is NOT NULL (presence bit is the source of truth).
  std::string c;
  EncodeValue(layout, {1.0, std::string("")}, "pc", c);
  std::string_view vc(c);
  AttrView cat = DecodeAttr(layout, vc, 1);
  EXPECT_FALSE(std::holds_alternative<std::monostate>(cat));
  EXPECT_EQ(std::get<std::string_view>(cat), "");
}

// Workload: 4-byte float ORDERED round-trip.
// Threat: float stored as truncated double bytes instead of IEEE754 single.
TEST(EncodeDecode, RoundTripFloat) {
  BitLSMOptions options;
  options.attr_num = 1;
  options.read_seqno = 0;
  options.rho = 0.5;
  options.attr_specs = {
      AttrSpec(AttrRole::ORDERED, 4, /*signed=*/true, /*is_float=*/true)};

  ValueLayout layout(options);
  EXPECT_EQ(layout.unordered_base, 4u);
  std::string out;
  EncodeValue(layout, {1.5}, "", out);
  EXPECT_EQ(out.size(), 4u);
  EXPECT_DOUBLE_EQ(
      std::get<double>(DecodeAttr(layout, std::string_view(out), 0)), 1.5);
}
