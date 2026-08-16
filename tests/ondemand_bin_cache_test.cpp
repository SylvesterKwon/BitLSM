#include <gtest/gtest.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"
#include "test_util/generators.h"

using namespace bit_lsm;

namespace {

// A strict-capacity cache sized (see TinyCacheStaysCorrect) to be too small
// for that test's one oversized bin while still holding the handful of data
// blocks its query needs to verify candidates -- so a refused bin insert is
// the only thing under test, not an unrelated data-block cache miss. Built
// the way udi_block_cache_test.cpp installs its own block cache.
rocksdb::BlockBasedTableOptions TinyStrictCacheOptions() {
  rocksdb::BlockBasedTableOptions table_options;
  table_options.block_cache =
      rocksdb::NewLRUCache(16384, /*num_shard_bits=*/0,
                           /*strict_capacity_limit=*/true);
  return table_options;
}

}  // namespace

// Threat: the point of the redesign is that a bigger budget buys fewer
// decodes. If the decoded-bin cache is bypassed (as PR #45's raw-page cache
// was), the second identical query re-loads every bin and this fails.
TEST_F(BitLSMTestBase, SecondQueryServesBinsFromCache) {
  // Seed 1 verified to draw a non-empty-clause query (below) for both q1 and
  // q2: the query touches real bins, so bitmaps_loaded > 0 cold is meant to
  // be deterministic once the scan path is wired to Bin() (Task 7).
  Rng rng(1);
  BitLSMOptions schema = GenerateSchema(rng);
  schema.ondemand_index = true;
  CheckedBitLSM db(&OpenDB(schema), schema);
  WorkloadParams workload;  // defaults; same shape diff_driver uses
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       GenerateAttrs(rng, schema, workload, db.reference()),
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  // Two byte-identical queries: generate from two copies of one rng state.
  Rng q1 = rng;
  Rng q2 = rng;
  ResetSABIBinCacheStats();
  ASSERT_TRUE(
      db.VerifyQuery(GenerateQuery(q1, schema, workload, db.reference())));
  SABIBinCacheStats first = GetSABIBinCacheStats();
  ASSERT_GT(first.bitmaps_loaded, 0u);  // cold: bins were read+decoded

  ASSERT_TRUE(
      db.VerifyQuery(GenerateQuery(q2, schema, workload, db.reference())));
  SABIBinCacheStats second = GetSABIBinCacheStats();
  EXPECT_EQ(second.bitmaps_loaded, first.bitmaps_loaded)
      << "warm query decoded bins again: the budget buys nothing";
  EXPECT_GT(second.hits, first.hits);
}

// Threat: pre-v7 the estimator counted rows by decoding every bin of every
// SST. If any consumer regresses to that, stats rebuild in ondemand mode
// pages the whole index back in.
TEST_F(BitLSMTestBase, EstimatorRebuildLoadsNoBitmaps) {
  Rng rng(1);
  BitLSMOptions schema = GenerateSchema(rng);
  schema.ondemand_index = true;
  schema.enable_estimator = true;
  // CheckedBitLSM does not expose Estimator(); keep the raw engine too.
  BitLSM& engine = OpenDB(schema);
  CheckedBitLSM db(&engine, schema);
  WorkloadParams workload;
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       GenerateAttrs(rng, schema, workload, db.reference()),
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  ResetSABIBinCacheStats();
  engine.Estimator()->TEST_Refresh();  // full stats rebuild over every SST
  SABIBinCacheStats after = GetSABIBinCacheStats();
  EXPECT_EQ(after.bitmaps_loaded, 0u)
      << "stats rebuild decoded bitmaps; use the persisted cardinalities";
}

// Threat: with a cache too small for even one bin, a refused insert must
// degrade to iterator-owned bins (correct, slower), never to wrong results.
//
// This does not use GenerateSchema/GenerateQuery's random CNF queries the
// way the other two tests in this file do. A shared block_cache backs both
// SABI bins and ordinary RocksDB data blocks, so a capacity small enough to
// refuse a randomly-sized bin (a few KB, per SecondQueryServesBinsFromCache)
// is also too small for a single ~4 KB data block, and that non-SABI insert
// failure is a hard iterator error upstream (RocksDB itself, unpatched --
// see PutDataBlockToCache in block_based_table_reader.cc): the test would
// fail for a reason that has nothing to do with the on-demand bin cache.
// Measured empirically (see task-8-report.md): the two floors don't move
// together, so a schema engineered to make ONE bin large relative to what a
// single-row-match query needs to verify opens a wide, reliable window.
// 200,000 rows on one ORDERED attr at the coarsest rho (0.5, few/big bins)
// puts one outlier value in a large bin (~28 KB observed) while an EQUAL
// query on that value only verifies a handful of data blocks (succeeds down
// to ~9 KB observed); 16 KB sits in the middle of that window. Full-scan
// verification is deliberately not exercised here: an unconditioned scan
// touches every data block regardless of bin size, so its cache floor scales
// with row count independent of anything under test, and VerifyFullScan
// elsewhere already covers full-scan correctness without cache starvation.
TEST_F(BitLSMTestBase, TinyCacheStaysCorrect) {
  Rng rng(2);
  BitLSMOptions schema;
  schema.attr_num = 1;
  schema.attr_specs = {AttrSpec{AttrRole::ORDERED}};
  schema.rho = 0.5;  // coarsest binning: fewest, largest bins
  schema.read_seqno = 0;
  schema.ondemand_index = true;
  BitLSM& engine = OpenDBWithTableOptions(schema, TinyStrictCacheOptions());
  CheckedBitLSM db(&engine, schema);
  const int kRows = 200000;
  const int kOutlierRow = kRows / 2;
  const double kOutlierValue = 99999.0;  // far outside the [0, 100) mass
  for (int i = 0; i < kRows; ++i) {
    double v = (i == kOutlierRow)
                   ? kOutlierValue
                   : std::uniform_real_distribution<double>(0.0, 100.0)(rng);
    ASSERT_TRUE(db.Put("k" + std::to_string(i), std::vector<Attr>{v},
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  // Matches exactly the outlier row: the bin covering it is refused (too big
  // for the tiny cache), so the fallback view has to be right, not just the
  // handful of data blocks fetched to confirm the one match.
  QueryCondition cond;
  cond.attr_idx = 0;
  cond.op = CompareOp::EQUAL;
  cond.value = kOutlierValue;
  BitLSMQuery one_match(std::vector<QueryCondition>{cond});
  ASSERT_TRUE(db.VerifyQuery(one_match));
}
