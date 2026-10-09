#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "bit_lsm_encoding.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include "sabi.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

// -0.0 stored, EQ +0.0 queried (and the reverse). The re-check treats
// them as equal, so the bitmap phase must not prune on distinct okeys.
TEST(OkeyZeroRegression, NegZeroRowSurvivesEqPosZeroPruning) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8, /*nullable=*/false)};
  o.rho = 0.5;
  const ValueLayout layout(o);

  SABIBuilder builder(SABISchema::FromOptions(o),
                      std::make_unique<ValueLayoutExtractor>(o));
  std::string row;
  EncodeValue(layout, {Attr(-0.0)}, "", row);
  builder.OnKeyAdded(rocksdb::Slice("k0"), UDIB::ValueType::kValue,
                     rocksdb::Slice(row));
  std::string scratch;
  UDIB::BlockHandle bh{0, 100};
  builder.AddIndexEntry(rocksdb::Slice("k0"), nullptr, bh, &scratch);
  rocksdb::Slice contents;
  ASSERT_TRUE(builder.Finish(&contents).ok());

  rocksdb::Slice blob(contents);
  SABIReader reader(blob);

  BitLSMQuery q(std::vector<QueryCondition>{{0, CompareOp::EQUAL, 0.0}});
  SABIQuery sq = EncodeQuery(q, o);
  EXPECT_TRUE(reader.QueryCanMatch(sq));  // must not be pruned

  // Symmetric case: store +0.0, query EQ -0.0
  SABIBuilder builder2(SABISchema::FromOptions(o),
                       std::make_unique<ValueLayoutExtractor>(o));
  EncodeValue(layout, {Attr(0.0)}, "", row);
  builder2.OnKeyAdded(rocksdb::Slice("k0"), UDIB::ValueType::kValue,
                      rocksdb::Slice(row));
  builder2.AddIndexEntry(rocksdb::Slice("k0"), nullptr, bh, &scratch);
  ASSERT_TRUE(builder2.Finish(&contents).ok());
  rocksdb::Slice blob2(contents);
  SABIReader reader2(blob2);
  BitLSMQuery q2(std::vector<QueryCondition>{{0, CompareOp::EQUAL, -0.0}});
  EXPECT_TRUE(reader2.QueryCanMatch(EncodeQuery(q2, o)));
}

// okeys near 2^63, where a double ulp is ~1024: boundary estimation must
// not round past the true min and max, or EQ(min)/EQ(max) prunes the SST.
TEST(OkeyZeroRegression, EqOnExactMinMaxIsNotPruned) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kInt, 8,
                           /*nullable=*/false)};  // i64
  o.rho = 0.5;
  const ValueLayout layout(o);

  SABIBuilder builder(SABISchema::FromOptions(o),
                      std::make_unique<ValueLayoutExtractor>(o));
  std::string row;
  // okey(600)=2^63+600 rounds up to +1024 (past the true min);
  // okey(1400)=2^63+1400 rounds down to +1024 (past the true max).
  const int64_t kVals[] = {600, 1000, 1400};
  for (int64_t x : kVals) {
    EncodeValue(layout, {Attr(x)}, "", row);
    builder.OnKeyAdded(rocksdb::Slice("k"), UDIB::ValueType::kValue,
                       rocksdb::Slice(row));
  }
  std::string scratch;
  UDIB::BlockHandle bh{0, 100};
  builder.AddIndexEntry(rocksdb::Slice("k"), nullptr, bh, &scratch);
  rocksdb::Slice contents;
  ASSERT_TRUE(builder.Finish(&contents).ok());
  rocksdb::Slice blob(contents);
  SABIReader reader(blob);

  for (int64_t x : {int64_t(600), int64_t(1400)}) {  // exactly min and max
    BitLSMQuery q(std::vector<QueryCondition>{{0, CompareOp::EQUAL, x}});
    EXPECT_TRUE(reader.QueryCanMatch(EncodeQuery(q, o))) << "value " << x;
  }
}
