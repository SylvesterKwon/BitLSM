#include <gtest/gtest.h>

#include <cstring>
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

// Mixed kRange + kEquality values survive an encode/decode round trip.
TEST(EncodeDecode, RoundTripMixedAttrs) {
  BitLSMOptions options =
      MakeOptions({AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                   AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)});
  const ValueLayout layout(options);
  std::string out;
  EncodeValue(layout, {3.14, std::string("apple")}, "payload", out);

  std::string_view buf(out);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 0)), 3.14);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 1)), "apple");
  EXPECT_EQ(DecodePayload(layout, buf), "payload");
}

// Two variable-length kEquality attrs (the non-last one needs a computed
// length) and an empty payload.
TEST(EncodeDecode, RoundTripMultipleUnorderedEmptyPayload) {
  BitLSMOptions options =
      MakeOptions({AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary),
                   AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary),
                   AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)});
  const ValueLayout layout(options);
  std::string out;
  EncodeValue(layout, {std::string("ab"), std::string("cdef"), 2.5}, "", out);

  std::string_view buf(out);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 0)), "ab");
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 1)), "cdef");
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 2)), 2.5);
  EXPECT_EQ(DecodePayload(layout, buf), "");
}

// No variable attrs, so the var_end header is zero bytes and the payload
// start comes from the schema alone.
TEST(EncodeDecode, RoundTripAllOrderedZeroHeader) {
  BitLSMOptions options =
      MakeOptions({AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                   AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)});
  const ValueLayout layout(options);
  std::string out;
  EncodeValue(layout, {1.5, -2.5}, "tail", out);

  EXPECT_EQ(layout.variable_base, 2 * sizeof(double));
  EXPECT_EQ(out.size(), 2 * sizeof(double) + 4);

  std::string_view buf(out);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 0)), 1.5);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 1)), -2.5);
  EXPECT_EQ(DecodePayload(layout, buf), "tail");
}

TEST(EncodeDecode, DoubleSchemaFixedLayout) {
  BitLSMOptions options =
      MakeOptions({AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                   AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)});
  const ValueLayout layout(options);
  std::string out;
  EncodeValue(layout, {3.14, std::string("apple")}, "pay", out);
  // [var_end u32][double 8B][cat bytes][payload]
  ASSERT_EQ(out.size(), sizeof(uint32_t) + sizeof(double) + 5 + 3);
  uint32_t var_end;
  std::memcpy(&var_end, out.data(), sizeof(uint32_t));
  EXPECT_EQ(var_end, 5u);  // "apple"
  double d;
  std::memcpy(&d, out.data() + sizeof(uint32_t), sizeof(double));
  EXPECT_DOUBLE_EQ(d, 3.14);
}

// int32/int64/uint32 slots, including negative sign-extension.
TEST(EncodeDecode, RoundTripNativeIntegers) {
  BitLSMOptions options;
  options.attr_num = 3;
  options.read_seqno = 0;
  options.rho = 0.5;
  options.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kInt, 4),
                        AttrSpec(IndexType::kRange, PhysicalType::kInt, 8),
                        AttrSpec(IndexType::kRange, PhysicalType::kUint, 4)};

  ValueLayout layout(options);
  // 4B + 8B + 4B fixed region, no unordered, no payload.
  EXPECT_EQ(layout.variable_base, 16u);

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

// A NULL in each position round-trips as monostate, and a NULL unordered
// is distinct from an empty string.
TEST(EncodeDecode, RoundTripNullAttrs) {
  BitLSMOptions options;
  options.attr_num = 2;
  options.read_seqno = 0;
  options.rho = 0.5;
  options.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8, /*nullable=*/true),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary, 0,
               /*nullable=*/true)};

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

  // An unordered empty string is not NULL; the presence bit decides.
  std::string c;
  EncodeValue(layout, {1.0, std::string("")}, "pc", c);
  std::string_view vc(c);
  AttrView cat = DecodeAttr(layout, vc, 1);
  EXPECT_FALSE(std::holds_alternative<std::monostate>(cat));
  EXPECT_EQ(std::get<std::string_view>(cat), "");
}

// A 4-byte float must be stored as IEEE754 single, not a truncated double.
TEST(EncodeDecode, RoundTripFloat) {
  BitLSMOptions options;
  options.attr_num = 1;
  options.read_seqno = 0;
  options.rho = 0.5;
  options.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kFloat, 4)};

  ValueLayout layout(options);
  EXPECT_EQ(layout.variable_base, 4u);
  std::string out;
  EncodeValue(layout, {1.5}, "", out);
  EXPECT_EQ(out.size(), 4u);
  EXPECT_DOUBLE_EQ(
      std::get<double>(DecodeAttr(layout, std::string_view(out), 0)), 1.5);
}

// The layout keys on physical type, not index type: the kEquality int
// takes a fixed slot and the kRange string the variable region.
TEST(EncodeDecode, LayoutKeysOnPhysicalType) {
  BitLSMOptions o =
      MakeOptions({AttrSpec(IndexType::kRange, PhysicalType::kBinary, 4),
                   AttrSpec(IndexType::kEquality, PhysicalType::kInt, 8),
                   AttrSpec(IndexType::kRange, PhysicalType::kVarBinary)});
  const ValueLayout layout(o);
  std::string out;
  EncodeValue(layout, {std::string("abcd"), int64_t(-2), std::string("xyz")},
              "PAY", out);
  // [var_end u32 = 3][abcd][int64 -2 LE][xyz][PAY]
  ASSERT_EQ(out.size(), 4u + 4u + 8u + 3u + 3u);
  uint32_t var_end;
  std::memcpy(&var_end, out.data(), 4);
  EXPECT_EQ(var_end, 3u);
  EXPECT_EQ(out.substr(4, 4), "abcd");
  EXPECT_EQ(out.substr(16, 3), "xyz");
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, out, 0)), "abcd");
  EXPECT_EQ(std::get<int64_t>(DecodeAttr(layout, out, 1)), -2);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, out, 2)), "xyz");
  EXPECT_EQ(DecodePayload(layout, out), "PAY");
}
