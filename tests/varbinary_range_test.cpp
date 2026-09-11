#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "sabi.h"
#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

BitLSMOptions VarBinaryRangeOpt(double rho, bool nullable = false) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kVarBinary, 0, nullable)};
  o.read_seqno = 0;
  o.rho = rho;
  return o;
}

BitLSMQuery Str(CompareOp op, std::string v) {
  return BitLSMQuery(std::vector<QueryCondition>{{0, op, std::move(v)}});
}

std::string Padded(const char* prefix, int i, int digits) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s%0*d", prefix, digits, i);
  return buf;
}

// One blob over `values` single-attr rows (no NULLs), parsed back.
struct BuiltIndex {
  std::unique_ptr<SABIBuilder> builder;
  std::string blob;
  std::unique_ptr<SABIReader> reader;
};
BuiltIndex BuildIndex(const BitLSMOptions& o,
                      const std::vector<std::string>& values) {
  BuiltIndex built;
  built.builder = std::make_unique<SABIBuilder>(
      SABISchema::FromOptions(o), std::make_unique<ValueLayoutExtractor>(o));
  std::vector<std::string> encoded;
  encoded.reserve(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    std::string row;
    EncodeValue(o, {values[i]}, "", row);
    encoded.push_back(std::move(row));
    built.builder->OnKeyAdded(rocksdb::Slice("k" + std::to_string(i)),
                              UDIB::ValueType::kValue,
                              rocksdb::Slice(encoded.back()));
  }
  std::string scratch;
  UDIB::BlockHandle bh{0, 100};
  built.builder->AddIndexEntry(rocksdb::Slice("k"), nullptr, bh, &scratch);
  rocksdb::Slice contents;
  EXPECT_TRUE(built.builder->Finish(&contents).ok());
  built.blob.assign(contents.data(), contents.size());
  rocksdb::Slice s(built.blob);
  built.reader = std::make_unique<SABIReader>(s);
  return built;
}

}  // namespace

// Workload: 60 VARCHAR-like values "user-00".."user-59" flushed once; every
//           operator with comparands on stored values, between stored values
//           ("user-295", "user-3"), the bare prefix, the empty string and
//           values above the maximum.
// Threat: byte-domain bin selection off by one at a boundary (open vs closed
//         upper bound, '\0' successor on a strict lower bound) drops rows the
//         re-check would have kept, or the re-check compares in a different
//         order than the bins were built in.
TEST_F(BitLSMTestBase, VarBinaryRangeAllOps) {
  BitLSMOptions opt = VarBinaryRangeOpt(0.2);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 60; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {Padded("user-", i, 2)}, "p"));
  ASSERT_TRUE(db.Flush());
  const std::vector<std::string> probes = {
      "user-00", "user-29", "user-295", "user-3", "user-59",
      "user-",   "",        "user-99",  "zzz"};
  for (const std::string& t : probes) {
    for (CompareOp op :
         {CompareOp::EQUAL, CompareOp::LESS, CompareOp::LESS_EQUAL,
          CompareOp::GREATER, CompareOp::GREATER_EQUAL})
      ASSERT_TRUE(db.VerifyQuery(Str(op, t))) << t;
  }
}

// Workload: BETWEEN-shaped CNF (x >= "user-10" AND x < "user-20") and a
//           two-member OR clause, over the same 60 rows, before and after
//           Flush and after a full compaction.
// Threat: same-attr clause merging into one ByteInterval, and OR members
//         staying separate, must hold on the byte domain exactly as on okeys.
TEST_F(BitLSMTestBase, VarBinaryBetweenAndOr) {
  BitLSMOptions opt = VarBinaryRangeOpt(0.2);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 60; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {Padded("user-", i, 2)}, "p"));
  BitLSMQuery between(std::vector<OrClause>{
      {{0, CompareOp::GREATER_EQUAL, std::string("user-10")}},
      {{0, CompareOp::LESS, std::string("user-20")}}});
  BitLSMQuery either(std::vector<OrClause>{
      {{0, CompareOp::LESS, std::string("user-05")},
       {0, CompareOp::GREATER_EQUAL, std::string("user-55")}}});
  ASSERT_TRUE(db.VerifyQuery(between));
  ASSERT_TRUE(db.VerifyQuery(either));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(between));
  ASSERT_TRUE(db.VerifyQuery(either));
  ASSERT_TRUE(db.CompactAll());
  ASSERT_TRUE(db.VerifyQuery(between));
  ASSERT_TRUE(db.VerifyQuery(either));
}

// Workload: nullable kRange kVarBinary; 40 valued rows, 10 NULL rows, 5
//           deletes, then range queries whose window covers the whole domain.
// Threat: NULL rows leaking into value bins or into range results (3VL),
//         tombstoned rows resurfacing through the byte-domain bins.
TEST_F(BitLSMTestBase, VarBinaryRangeWithNullsAndDeletes) {
  BitLSMOptions opt = VarBinaryRangeOpt(0.2, /*nullable=*/true);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 40; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {Padded("v", i, 2)}, "p"));
  for (int i = 40; i < 50; ++i)
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {std::monostate{}}, "p"));
  ASSERT_TRUE(db.Flush());
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(db.Delete("k" + std::to_string(i)));
  ASSERT_TRUE(db.Flush());
  ASSERT_TRUE(db.VerifyQuery(Str(CompareOp::GREATER_EQUAL, "")));
  ASSERT_TRUE(db.VerifyQuery(Str(CompareOp::LESS, "v20")));
  ASSERT_TRUE(db.VerifyQuery(Str(CompareOp::EQUAL, "v03")));
}

// Workload: 1000 distinct values "prefix-0000".."prefix-0999" sharing a
//           7-byte common prefix, rho 0.01 (100 bins), built into one blob.
// Threat: a binning that projected strings through a fixed-width numeric
//         prefix would collapse common-prefix data into one bin and lose all
//         pruning; byte-string boundaries must split the run into 100 equal
//         bins pinned to the exact min and max.
TEST(VarBinaryBinning, CommonPrefixDataSpreadsAcrossBins) {
  std::vector<std::string> values;
  for (int i = 0; i < 1000; ++i) values.push_back(Padded("prefix-", i, 4));
  BuiltIndex built = BuildIndex(VarBinaryRangeOpt(0.01), values);
  const SABIReader& reader = *built.reader;
  const uint32_t bins = reader.bitmap_index.bitmap_nums[0];
  ASSERT_EQ(bins, 100u);
  const BytesList& b =
      std::get<BytesList>(reader.bitmap_index.binning_policy[0]);
  ASSERT_EQ(b.size(), bins + 1);
  EXPECT_EQ(b[0], "prefix-0000");
  EXPECT_EQ(b.back(), "prefix-0999");
  for (uint32_t i = 1; i < b.size(); ++i) EXPECT_LE(b[i - 1], b[i]);
  for (uint32_t bin = 0; bin < bins; ++bin)
    EXPECT_EQ(reader.BinCardinality(bin), 10u) << "bin " << bin;
}

// Workload: the blob above; SelectBins for <, <=, >, >= with the comparand
//           sitting exactly on an interior boundary B_j.
// Threat: an open upper bound resolved with upper_bound selects the bin
//         starting at B_j for x < B_j (a wasted bin); a closed bound resolved
//         with lower_bound drops that bin for x <= B_j (a false negative).
TEST(VarBinaryBinning, SelectBinsAtExactBoundary) {
  std::vector<std::string> values;
  for (int i = 0; i < 1000; ++i) values.push_back(Padded("prefix-", i, 4));
  BuiltIndex built = BuildIndex(VarBinaryRangeOpt(0.01), values);
  const SABIReader& reader = *built.reader;
  const BytesList& b =
      std::get<BytesList>(reader.bitmap_index.binning_policy[0]);
  const uint32_t j = 37;
  const std::string bj(b[j]);
  auto sel = [&](CompareOp op) {
    SABICondition c{0, ByteInterval::FromOp(op, bj), ""};
    BinSelection out;
    EXPECT_TRUE(reader.SelectBins(c, &out));
    return out;
  };
  EXPECT_EQ(sel(CompareOp::LESS).last, j - 1);
  EXPECT_EQ(sel(CompareOp::LESS_EQUAL).last, j);
  EXPECT_EQ(sel(CompareOp::GREATER).first, j);
  EXPECT_EQ(sel(CompareOp::GREATER_EQUAL).first, j);
  EXPECT_EQ(sel(CompareOp::EQUAL).first, j);
  EXPECT_EQ(sel(CompareOp::EQUAL).last, j);
}
