#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "bit_lsm_encoding.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include "sabi.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

// Workload: store -0.0 in an SST, query EQ +0.0 through QueryCanMatch (and
//           the symmetric +0.0 / EQ -0.0 pair).
// Threat: native == treats ±0.0 as equal, so the row re-check would match —
//         if the bitmap phase prunes on distinct okeys, the row is silently
//         lost (false negative). F64ToOkey's -0.0 canonicalization guards
//         this.
TEST(OkeyZeroRegression, NegZeroRowSurvivesEqPosZeroPruning) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, true, false)};
  o.rho = 0.5;

  SABIBuilder builder(SABISchema::FromOptions(o),
                      std::make_unique<ValueLayoutExtractor>(o));
  std::string row;
  EncodeValue(o, {Attr(-0.0)}, "", row);
  builder.OnKeyAdded(rocksdb::Slice("k0"), UDIB::ValueType::kValue,
                     rocksdb::Slice(row));
  std::string scratch;
  UDIB::BlockHandle bh{0, 100};
  builder.AddIndexEntry(rocksdb::Slice("k0"), nullptr, bh, &scratch);
  rocksdb::Slice contents;
  ASSERT_TRUE(builder.Finish(&contents).ok());

  rocksdb::Slice blob(contents);
  SABIReader reader(blob, SABISchema::FromOptions(o));

  BitLSMQuery q(std::vector<QueryCondition>{{0, CompareOp::EQUAL, 0.0}});
  SABIQuery sq = EncodeQuery(q, o);
  EXPECT_TRUE(reader.QueryCanMatch(sq));  // must NOT be pruned

  // Symmetric case: store +0.0, query EQ -0.0
  SABIBuilder builder2(SABISchema::FromOptions(o),
                       std::make_unique<ValueLayoutExtractor>(o));
  EncodeValue(o, {Attr(0.0)}, "", row);
  builder2.OnKeyAdded(rocksdb::Slice("k0"), UDIB::ValueType::kValue,
                      rocksdb::Slice(row));
  builder2.AddIndexEntry(rocksdb::Slice("k0"), nullptr, bh, &scratch);
  ASSERT_TRUE(builder2.Finish(&contents).ok());
  rocksdb::Slice blob2(contents);
  SABIReader reader2(blob2, SABISchema::FromOptions(o));
  BitLSMQuery q2(std::vector<QueryCondition>{{0, CompareOp::EQUAL, -0.0}});
  EXPECT_TRUE(reader2.QueryCanMatch(EncodeQuery(q2, o)));
}

// Workload: i64 values 600/1000/1400 (okeys near 2^63 where a double ulp is
//           ~1024), then EQ queries on the exact min and max stored values.
// Threat: the okey->double->okey round trip in boundary estimation can round
//         PAST the true data bounds; without min/max pinning the SST-level
//         pruning (ConditionImpossible) drops the whole SST on EQ(min)/EQ(max)
//         (false negative).
TEST(OkeyZeroRegression, EqOnExactMinMaxIsNotPruned) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, false, false)};  // i64
  o.rho = 0.5;

  SABIBuilder builder(SABISchema::FromOptions(o),
                      std::make_unique<ValueLayoutExtractor>(o));
  std::string row;
  // okey(600)=2^63+600 rounds up to +1024 (past the true min);
  // okey(1400)=2^63+1400 rounds down to +1024 (past the true max).
  const int64_t kVals[] = {600, 1000, 1400};
  for (int64_t x : kVals) {
    EncodeValue(o, {Attr(x)}, "", row);
    builder.OnKeyAdded(rocksdb::Slice("k"), UDIB::ValueType::kValue,
                       rocksdb::Slice(row));
  }
  std::string scratch;
  UDIB::BlockHandle bh{0, 100};
  builder.AddIndexEntry(rocksdb::Slice("k"), nullptr, bh, &scratch);
  rocksdb::Slice contents;
  ASSERT_TRUE(builder.Finish(&contents).ok());
  rocksdb::Slice blob(contents);
  SABIReader reader(blob, SABISchema::FromOptions(o));

  for (int64_t x : {int64_t(600), int64_t(1400)}) {  // exactly min and max
    BitLSMQuery q(std::vector<QueryCondition>{{0, CompareOp::EQUAL, x}});
    EXPECT_TRUE(reader.QueryCanMatch(EncodeQuery(q, o))) << "value " << x;
  }
}
