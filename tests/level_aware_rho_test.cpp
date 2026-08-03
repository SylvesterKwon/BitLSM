#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/user_defined_index.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "sabi.h"
#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

// {ORDERED double, UNORDERED} — the common 2-attr schema.
BitLSMOptions TwoAttrOptions(double rho, double gamma) {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec{AttrRole::ORDERED}, AttrSpec{AttrRole::UNORDERED}};
  o.read_seqno = 0;
  o.rho = rho;
  o.gamma = gamma;
  return o;
}

// Single ORDERED double attr — bin budget is fully deterministic:
// bins = min((uint32_t)(1 / rho_effective), cardinality).
BitLSMOptions OneAttrOptions(double rho, double gamma) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec{AttrRole::ORDERED}};
  o.read_seqno = 0;
  o.rho = rho;
  o.gamma = gamma;
  return o;
}

// Feeds `rows` distinct ordered values (row i -> attr0 = i, attr1 = "v<i%7>"
// when the schema has 2 attrs) into `builder` and finishes the blob. The blob
// memory is owned by the builder — keep it alive while reading the slice.
void BuildBlob(SABIBuilder& builder, const BitLSMOptions& options,
               uint32_t rows, rocksdb::Slice* blob) {
  std::vector<std::string> encoded(rows);
  for (uint32_t i = 0; i < rows; ++i) {
    std::vector<Attr> attrs = {static_cast<double>(i)};
    if (options.attr_num == 2) attrs.push_back("v" + std::to_string(i % 7));
    EncodeValue(options, attrs, "p", encoded[i]);
    builder.OnKeyAdded(rocksdb::Slice("k" + std::to_string(i)),
                       UDIB::ValueType::kValue, rocksdb::Slice(encoded[i]));
  }
  std::string scratch;
  UDIB::BlockHandle handle{/*offset=*/100, /*size=*/4096};
  builder.AddIndexEntry(rocksdb::Slice("k" + std::to_string(rows - 1)),
                        /*first_key_in_next_block=*/nullptr, handle, &scratch);
  BITLSM_ASSERT_OK(builder.Finish(blob));
}

}  // namespace

// Workload: the same 64-row sequence built three ways — legacy 2-arg
//           constructor, gamma=1.0 with d=0, gamma=1.0 with d=6 — at rho 0.1
//           and at rho 0.5 (the clamp boundary; also above-clamp rho 0.7,
//           legal at gamma=1).
// Threat: the gamma wiring or the rho_effective computation perturbs the
//         gamma=1 path (extra pow/clamp rounding), silently changing SABI
//         bytes of every existing DB and breaking the regression baseline.
TEST(LevelAwareRho, GammaOneIsByteIdentical) {
  for (double rho : {0.1, 0.5, 0.7}) {
    BitLSMOptions options = TwoAttrOptions(rho, /*gamma=*/1.0);
    SABISchema schema = SABISchema::FromOptions(options);

    SABIBuilder legacy(schema, std::make_unique<ValueLayoutExtractor>(options));
    SABIBuilder at_bottom(schema,
                          std::make_unique<ValueLayoutExtractor>(options),
                          /*level_d=*/0);
    SABIBuilder at_top(schema, std::make_unique<ValueLayoutExtractor>(options),
                       /*level_d=*/6);

    rocksdb::Slice blob_legacy, blob_bottom, blob_top;
    BuildBlob(legacy, options, 64, &blob_legacy);
    BuildBlob(at_bottom, options, 64, &blob_bottom);
    BuildBlob(at_top, options, 64, &blob_top);

    ASSERT_EQ(blob_legacy.size(), blob_bottom.size()) << "rho=" << rho;
    ASSERT_EQ(blob_legacy.size(), blob_top.size()) << "rho=" << rho;
    EXPECT_EQ(
        0, memcmp(blob_legacy.data(), blob_bottom.data(), blob_legacy.size()))
        << "rho=" << rho;
    EXPECT_EQ(0,
              memcmp(blob_legacy.data(), blob_top.data(), blob_legacy.size()))
        << "rho=" << rho;
  }
}

// Workload: single ORDERED attr, rho=0.05, gamma=2, 1000 distinct values;
//           one build per distance d = 0..4.
// Threat: decay applied in the wrong direction (shallower levels getting a
//         larger budget), a missing 0.5 clamp letting the bin count drop
//         below 2, or d not reaching the builder at all (every level gets the
//         same budget).
TEST(LevelAwareRho, GammaDecayShrinksBudgetByLevel) {
  const std::vector<uint32_t> expected_bins = {20, 10, 5, 2, 2};
  for (uint32_t d = 0; d < expected_bins.size(); ++d) {
    BitLSMOptions options = OneAttrOptions(/*rho=*/0.05, /*gamma=*/2.0);
    SABIBuilder builder(SABISchema::FromOptions(options),
                        std::make_unique<ValueLayoutExtractor>(options), d);
    rocksdb::Slice blob;
    BuildBlob(builder, options, 1000, &blob);
    SABIReader reader(blob);
    ASSERT_EQ(reader.bitmap_index.bitmap_nums.size(), 1u);
    EXPECT_EQ(reader.bitmap_index.bitmap_nums[0], expected_bins[d])
        << "d=" << d;
  }
}

// Workload: Validate() on gamma below 1, NaN, infinity, the legal 1.0 / 3.0
//           values, and the inverted gamma>1 && rho>0.5 combination; then a
//           real DB open with an invalid gamma.
// Threat: an inverted-decay configuration passes silently and shallower
//         levels get a *larger* budget; NaN makes every comparison false and
//         sails through; the constructor path skips validation entirely.
TEST(LevelAwareRho, ValidationRejectsInvertedDecay) {
  BitLSMOptions o = TwoAttrOptions(/*rho=*/0.1, /*gamma=*/1.0);
  EXPECT_TRUE(o.Validate().ok());
  o.gamma = 3.0;
  EXPECT_TRUE(o.Validate().ok());
  o.gamma = 0.5;
  EXPECT_TRUE(o.Validate().IsInvalidArgument());
  o.gamma = NAN;
  EXPECT_TRUE(o.Validate().IsInvalidArgument());
  o.gamma = INFINITY;
  EXPECT_TRUE(o.Validate().IsInvalidArgument());

  // gamma > 1 with rho above the clamp would invert the decay direction.
  o = TwoAttrOptions(/*rho=*/0.7, /*gamma=*/2.0);
  EXPECT_TRUE(o.Validate().IsInvalidArgument());
  o.gamma = 1.0;  // same rho is legal while decay is off
  EXPECT_TRUE(o.Validate().ok());
}

// Workload: OpenDB with gamma=0.5.
// Threat: BitLSM construction ignores Validate() and opens a DB that decays
//         upward.
TEST_F(BitLSMTestBase, OpenRejectsInvalidGamma) {
  BitLSMOptions o = DefaultOptions();
  o.gamma = 0.5;
  EXPECT_THROW(OpenDB(o), std::invalid_argument);
}

// Workload: LevelDistanceFromDeepest over known, unknown (-1), and
//           out-of-range levels, plus a factory NewBuilder(option) build with
//           an unknown level under gamma=2.
// Threat: an SstFileWriter-style build (level unknown) computes a garbage
//         distance (negative cast blow-up) instead of the conservative
//         maximum-decay fallback.
TEST(LevelAwareRho, UnknownLevelFallsBackToMaxDecay) {
  EXPECT_EQ(LevelDistanceFromDeepest(-1, -1), 6u);
  EXPECT_EQ(LevelDistanceFromDeepest(-1, 7), 6u);
  EXPECT_EQ(LevelDistanceFromDeepest(0, 7), 6u);
  EXPECT_EQ(LevelDistanceFromDeepest(3, 7), 3u);
  EXPECT_EQ(LevelDistanceFromDeepest(6, 7), 0u);
  EXPECT_EQ(LevelDistanceFromDeepest(9, 7), 6u);  // out of range = unknown
  EXPECT_EQ(LevelDistanceFromDeepest(2, 5), 2u);

  // rho 0.05 * 2^6 = 3.2 -> clamped to 0.5 -> 2 bins (vs 20 at the bottom).
  BitLSMOptions options = OneAttrOptions(/*rho=*/0.05, /*gamma=*/2.0);
  SABIFactory factory(SABISchema::FromOptions(options), [options] {
    return std::make_unique<ValueLayoutExtractor>(options);
  });
  rocksdb::UserDefinedIndexOption udi_option;
  udi_option.level_at_creation = -1;
  udi_option.num_levels = -1;
  std::unique_ptr<UDIB> builder;
  BITLSM_ASSERT_OK(factory.NewBuilder(udi_option, builder));

  std::vector<std::string> encoded(100);
  for (uint32_t i = 0; i < 100; ++i) {
    EncodeValue(options, {static_cast<double>(i)}, "p", encoded[i]);
    builder->OnKeyAdded(rocksdb::Slice("k" + std::to_string(i)),
                        UDIB::ValueType::kValue, rocksdb::Slice(encoded[i]));
  }
  std::string scratch;
  UDIB::BlockHandle handle{/*offset=*/100, /*size=*/4096};
  builder->AddIndexEntry(rocksdb::Slice("k99"), nullptr, handle, &scratch);
  rocksdb::Slice blob;
  BITLSM_ASSERT_OK(builder->Finish(&blob));
  SABIReader reader(blob);
  ASSERT_EQ(reader.bitmap_index.bitmap_nums.size(), 1u);
  EXPECT_EQ(reader.bitmap_index.bitmap_nums[0], 2u);
}

// Workload: gamma=2 DB; 300 rows flushed to L0 (d=6, minimum budget), then
//           compacted to the bottommost level (d=0, full budget); queries
//           verified against the oracle after each shape change.
// Threat: the level context RocksDB hands to the factory is wrong end-to-end
//         (flush not treated as maximum distance, compaction output level not
//         reaching the builder), or a decayed budget breaks result exactness
//         instead of only costing candidate precision.
TEST_F(BitLSMTestBase, DecayedBudgetsStayExactAcrossCompaction) {
  BitLSMOptions opt = TwoAttrOptions(/*rho=*/0.1, /*gamma=*/2.0);
  CheckedBitLSM db(&OpenDB(opt), opt);
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       {static_cast<double>(i), "v" + std::to_string(i % 5)},
                       "p"));
  }
  ASSERT_TRUE(db.Flush());  // L0 SST: built with d = num_levels - 1
  ASSERT_TRUE(db.VerifyQuery(
      BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::LESS, 150.0}})));
  ASSERT_TRUE(db.VerifyFullScan());

  ASSERT_TRUE(db.CompactAll());
  // dynamic_level_bytes sends the small dataset straight to the bottommost
  // level, so the rebuilt SST is a d=0 (full budget) build.
  std::vector<rocksdb::LiveFileMetaData> files;
  db_->GetInternalDB()->GetLiveFilesMetaData(&files);
  ASSERT_FALSE(files.empty());
  for (const auto& f : files) EXPECT_EQ(f.level, 6);

  ASSERT_TRUE(db.VerifyQuery(
      BitLSMQuery(std::vector<QueryCondition>{{0, CompareOp::LESS, 150.0}})));
  ASSERT_TRUE(db.VerifyQuery(BitLSMQuery(
      std::vector<QueryCondition>{{1, CompareOp::EQUAL, std::string("v3")}})));
  ASSERT_TRUE(db.VerifyFullScan());
}
