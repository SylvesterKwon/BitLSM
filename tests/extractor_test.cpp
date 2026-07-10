#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_utils.h"

using namespace bit_lsm;

// Workload: a v3 row with [ORDERED double, UNORDERED bytes, ORDERED i64]
//           attrs, extracted through ValueLayoutExtractor.
// Threat: extractor output diverging from DecodeAttr + OrderedToOkey would
//         put rows into different bins than the query path expects.
TEST(ValueLayoutExtractor, MatchesDecodeAttr) {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, true, true),  // double
                  AttrSpec(AttrRole::UNORDERED),                     // bytes
                  AttrSpec(AttrRole::ORDERED, 8, true, false, false)};  // i64

  std::vector<Attr> attrs = {Attr(3.25), Attr(std::string("seoul")),
                             Attr(int64_t(-42))};
  std::string row;
  EncodeValue(o, attrs, "payload", row);

  ValueLayoutExtractor ex(o);
  std::vector<EncodedAttr> out(o.attr_num);
  ex.ExtractAll("pk0", row, out.data());

  EXPECT_EQ(std::get<uint64_t>(out[0]), F64ToOkey(3.25));
  EXPECT_EQ(std::get<std::string_view>(out[1]), "seoul");
  EXPECT_EQ(std::get<uint64_t>(out[2]), I64ToOkey(-42));
}

// Workload: a v3 row whose nullable ORDERED attr is SQL NULL.
// Threat: a NULL leaking through as okey 0 (instead of monostate) would land
//         the row in a value bin and match value predicates it must not.
TEST(ValueLayoutExtractor, NullBecomesMonostate) {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, true, true),
                  AttrSpec(AttrRole::UNORDERED)};
  std::vector<Attr> attrs = {Attr(std::monostate{}), Attr(std::string("x"))};
  std::string row;
  EncodeValue(o, attrs, "", row);

  ValueLayoutExtractor ex(o);
  std::vector<EncodedAttr> out(o.attr_num);
  ex.ExtractAll("pk0", row, out.data());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(out[0]));
  EXPECT_EQ(std::get<std::string_view>(out[1]), "x");
}
