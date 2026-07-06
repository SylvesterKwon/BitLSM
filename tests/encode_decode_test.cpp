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
  BitLSMOptions options = MakeOptions({AttrType::ORDERED, AttrType::UNORDERED});
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
      {AttrType::UNORDERED, AttrType::UNORDERED, AttrType::ORDERED});
  std::string out;
  EncodeValue(options, {std::string("ab"), std::string("cdef"), 2.5}, "", out);

  ValueLayout layout(options);
  std::string_view buf(out);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 0)), "ab");
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(layout, buf, 1)), "cdef");
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 2)), 2.5);
  EXPECT_EQ(DecodePayload(layout, buf), "");
}

// Workload: all-ordered schema (n_cat == 0) with a payload.
// Threat: the v2 header is zero bytes in this case — payload start must be
//         derived purely from the schema (8B x n_cont), and an off-by-one
//         there corrupts every attribute and the payload.
TEST(EncodeDecode, RoundTripAllOrderedZeroHeader) {
  BitLSMOptions options = MakeOptions({AttrType::ORDERED, AttrType::ORDERED});
  std::string out;
  EncodeValue(options, {1.5, -2.5}, "tail", out);

  ValueLayout layout(options);
  EXPECT_EQ(layout.cat_base, 2 * sizeof(double));
  EXPECT_EQ(out.size(), 2 * sizeof(double) + 4);

  std::string_view buf(out);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 0)), 1.5);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(layout, buf, 1)), -2.5);
  EXPECT_EQ(DecodePayload(layout, buf), "tail");
}
