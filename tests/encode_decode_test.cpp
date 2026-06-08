#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"

using namespace bit_lsm;

namespace {
BitLSMOptions MakeOptions(std::vector<AttrType> types) {
  BitLSMOptions options;
  options.attr_num = static_cast<uint32_t>(types.size());
  options.attr_types = std::move(types);
  options.read_seqno = 0;
  options.rho = 0.5;
  return options;
}
}  // namespace

// 연속형 + 범주형 혼합 값이 왕복(encode→decode)에서 보존되는지.
TEST(EncodeDecode, RoundTripMixedAttrs) {
  BitLSMOptions options =
      MakeOptions({AttrType::CONTINUOUS, AttrType::CATEGORICAL});
  std::string out;
  EncodeValue(options, {3.14, std::string("apple")}, "payload", out);

  std::string_view buf(out);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(AttrType::CONTINUOUS, buf, 0)),
                   3.14);
  EXPECT_EQ(
      std::get<std::string_view>(DecodeAttr(AttrType::CATEGORICAL, buf, 1)),
      "apple");
}

// 가변 길이 범주형 두 개(마지막이 아닌 범주형의 길이 계산)와 빈 payload.
TEST(EncodeDecode, RoundTripMultipleCategoricalEmptyPayload) {
  BitLSMOptions options = MakeOptions(
      {AttrType::CATEGORICAL, AttrType::CATEGORICAL, AttrType::CONTINUOUS});
  std::string out;
  EncodeValue(options, {std::string("ab"), std::string("cdef"), 2.5}, "", out);

  std::string_view buf(out);
  EXPECT_EQ(
      std::get<std::string_view>(DecodeAttr(AttrType::CATEGORICAL, buf, 0)),
      "ab");
  EXPECT_EQ(
      std::get<std::string_view>(DecodeAttr(AttrType::CATEGORICAL, buf, 1)),
      "cdef");
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(AttrType::CONTINUOUS, buf, 2)),
                   2.5);
}
