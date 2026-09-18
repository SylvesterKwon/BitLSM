#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_utils.h"

using namespace bit_lsm;

// Workload: a v3 row with [kRange double, kEquality bytes, kRange i64]
//           attrs, extracted through ValueLayoutExtractor.
// Threat: extractor output diverging from DecodeAttr + NumericToOkey (as
//         8-byte okey bytes) would put rows into different bins than the
//         query path expects.
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

// Workload: a v3 row whose nullable kRange attr is SQL NULL.
// Threat: a NULL leaking through as okey 0 (instead of monostate) would land
//         the row in a value bin and match value predicates it must not.
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
