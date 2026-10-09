#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_utils.h"

using namespace bit_lsm;

// Extractor output must equal DecodeAttr + NumericToOkey as 8-byte okey
// bytes, or rows land in different bins than queries expect.
TEST(ValueLayoutExtractor, MatchesDecodeAttr) {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8,
               /*nullable=*/true),                               // double
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary),  // bytes
      AttrSpec(IndexType::kRange, PhysicalType::kInt, 8,
               /*nullable=*/false)};  // i64

  std::vector<Attr> attrs = {Attr(3.25), Attr(std::string("seoul")),
                             Attr(int64_t(-42))};
  const ValueLayout layout(o);
  std::string row;
  EncodeValue(layout, attrs, "payload", row);

  ValueLayoutExtractor ex(o);
  std::vector<EncodedAttr> out(o.attr_num);
  ex.ExtractAll("pk0", row, out.data());

  EXPECT_EQ(std::get<std::string_view>(out[0]), OkeyToBytes(F64ToOkey(3.25)));
  EXPECT_EQ(std::get<std::string_view>(out[1]), "seoul");
  EXPECT_EQ(std::get<std::string_view>(out[2]), OkeyToBytes(I64ToOkey(-42)));
}

// A NULL leaking through as okey 0 would land the row in a value bin.
TEST(ValueLayoutExtractor, NullBecomesMonostate) {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8, /*nullable=*/true),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  std::vector<Attr> attrs = {Attr(std::monostate{}), Attr(std::string("x"))};
  const ValueLayout layout(o);
  std::string row;
  EncodeValue(layout, attrs, "", row);

  ValueLayoutExtractor ex(o);
  std::vector<EncodedAttr> out(o.attr_num);
  ex.ExtractAll("pk0", row, out.data());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(out[0]));
  EXPECT_EQ(std::get<std::string_view>(out[1]), "x");
}
