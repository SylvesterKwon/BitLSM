#include <gtest/gtest.h>
#include <rocksdb/cache.h>
#include <rocksdb/table.h>

#include <random>
#include <string>
#include <vector>

#include "block_prefetch_queue.h"
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
// decodes. If the decoded-bin cache is bypassed (as the v1 design's raw-page
// cache was), the second identical query re-loads every bin and this fails.
TEST_F(BitLSMTestBase, SecondQueryServesBinsFromCache) {
  // Seed 1 verified to draw a non-empty-clause query (below) for both q1 and
  // q2: the query touches real bins, so bitmaps_loaded > 0 cold is meant to
  // be deterministic because the scan path resolves bins through Bin().
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

// Threat: a range predicate's bins are contiguous in the blob, yet a cold
// query that loads each with its own pread pays ~one seek per bin (measured:
// serial ~1.2KB reads at fine rho turn one cold query into tens of seconds).
// Coalescing must make cold loading span-granular: fewer file reads than
// decoded bins, and a warm repeat must hit without any new read.
TEST_F(BitLSMTestBase, RangeQueryCoalescesBinReads) {
  Rng rng(3);
  BitLSMOptions schema;
  schema.attr_num = 1;
  schema.attr_specs = {AttrSpec{AttrRole::ORDERED}};
  schema.rho = 0.05;  // finest binning tier: ~20 bins on one attr
  schema.read_seqno = 0;
  schema.ondemand_index = true;
  CheckedBitLSM db(&OpenDB(schema), schema);
  for (int i = 0; i < 5000; ++i) {
    double v = std::uniform_real_distribution<double>(0.0, 100.0)(rng);
    ASSERT_TRUE(db.Put("k" + std::to_string(i), std::vector<Attr>{v},
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  // >= 10.0 over uniform [0, 100) spans nearly every bin of the attr.
  QueryCondition cond;
  cond.attr_idx = 0;
  cond.op = CompareOp::GREATER_EQUAL;
  cond.value = 10.0;
  BitLSMQuery range_query(std::vector<QueryCondition>{cond});

  ResetSABIBinCacheStats();
  ASSERT_TRUE(db.VerifyQuery(range_query));
  SABIBinCacheStats cold = GetSABIBinCacheStats();
  // Measured at this seed: reads=2 (tombstone + one 18-bin coalesced run),
  // bitmaps_loaded=19.
  // Positive control: the range really decoded several bins (plus the
  // tombstone); with fewer, reads < bitmaps_loaded could hold vacuously.
  ASSERT_GT(cold.bitmaps_loaded, 2u)
      << "range spanned too few bins to exercise coalescing";
  EXPECT_LT(cold.reads, cold.bitmaps_loaded)
      << "cold range query issued one read per bin: no coalescing happened";

  ASSERT_TRUE(db.VerifyQuery(range_query));
  SABIBinCacheStats warm = GetSABIBinCacheStats();
  EXPECT_EQ(warm.reads, cold.reads)
      << "warm repeat issued new file reads instead of hitting the cache";
  EXPECT_EQ(warm.bitmaps_loaded, cold.bitmaps_loaded)
      << "warm repeat re-decoded bins";
  EXPECT_GT(warm.hits, cold.hits);
}

// Threat: a multi-condition query's cold runs live in different attr regions
// of the blob (plus the tombstone), so span coalescing cannot merge them and
// BuildQueryBitmap would pread them one at a time, serially. The span
// prefetch must engage on such a query (plan >= 2 miss runs and submit
// them), replace preads one-for-one when async reads work, and stay
// behavior-identical when they don't -- this suite also runs on builds with
// no liburing, where every submit is refused and every run falls back to
// its own sync pread.
TEST_F(BitLSMTestBase, MultiAttrColdRunsPlanSpanPrefetch) {
  Rng rng(4);
  BitLSMOptions schema;
  schema.attr_num = 2;
  schema.attr_specs = {AttrSpec{AttrRole::ORDERED},
                       AttrSpec{AttrRole::ORDERED}};
  schema.rho = 0.05;  // finest binning tier: ~20 bins per attr
  schema.read_seqno = 0;
  schema.ondemand_index = true;
  CheckedBitLSM db(&OpenDB(schema), schema);
  for (int i = 0; i < 5000; ++i) {
    double v0 = std::uniform_real_distribution<double>(0.0, 100.0)(rng);
    double v1 = std::uniform_real_distribution<double>(0.0, 100.0)(rng);
    ASSERT_TRUE(db.Put("k" + std::to_string(i), std::vector<Attr>{v0, v1},
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  // Two range predicates whose bin runs sit far apart in the flat bitmap
  // region: attr 0's run ends at attr 0's last bin, attr 1's starts around
  // its middle, so a gap of unwanted bins separates them and no
  // byte-contiguous merge is possible between the conditions.
  QueryCondition c0, c1;
  c0.attr_idx = 0;
  c0.op = CompareOp::GREATER_EQUAL;
  c0.value = 10.0;
  c1.attr_idx = 1;
  c1.op = CompareOp::GREATER_EQUAL;
  c1.value = 50.0;
  BitLSMQuery query(std::vector<QueryCondition>{c0, c1});

  ResetSABIBinCacheStats();
  // Clears the process-wide one-way async-unavailable flag another test may
  // have tripped, so the plan phase runs here whatever the test order.
  ResetBlockPrefetchQueueStats();
  ASSERT_TRUE(db.VerifyQuery(query));  // the actual correctness bar
  SABIBinCacheStats cold = GetSABIBinCacheStats();
  // The plan/submit path engaged on every build: at least attr 0's run and
  // attr 1's (the latter possibly merged with the adjacent tombstone bin)
  // were computed and handed to submission.
  EXPECT_GE(cold.spans_planned, 2u)
      << "multi-attr cold query did not engage the span-prefetch plan";
  // Behavior-identical across builds: each of the three cold loads
  // (tombstone + one coalesced run per condition) happens exactly once, by
  // pread or out of a prefetch buffer -- never both, never neither.
  EXPECT_EQ(cold.reads + cold.spans_prefetched, 3u);
  if (GetBlockPrefetchQueueStats().async_unavailable) {
    // No liburing: every submit was refused, every run took the sync pread
    // fallback.
    EXPECT_EQ(cold.spans_prefetched, 0u);
  } else {
    EXPECT_GT(cold.spans_prefetched, 0u)
        << "async reads available but no run was served from a buffer";
  }

  // Warm repeat: every bin is cached, so the plan finds no miss runs (no
  // new engagement) and nothing is read again on either path.
  ASSERT_TRUE(db.VerifyQuery(query));
  SABIBinCacheStats warm = GetSABIBinCacheStats();
  EXPECT_EQ(warm.reads, cold.reads) << "warm repeat issued new file reads";
  EXPECT_EQ(warm.bitmaps_loaded, cold.bitmaps_loaded)
      << "warm repeat re-decoded bins";
  EXPECT_EQ(warm.spans_planned, cold.spans_planned)
      << "warm repeat re-planned runs that are already cached";
  EXPECT_EQ(warm.spans_prefetched, cold.spans_prefetched);
  EXPECT_GT(warm.hits, cold.hits);
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
  // Drain the flush-completion listener's own rebuild first. Without this,
  // the measured TEST_Refresh() below races that background pass: if it
  // already ran, Reconcile() sees fresh_enough == true and skips Rebuild()
  // entirely, and bitmaps_loaded == 0 would prove nothing (no rebuild ran at
  // all, let alone one that avoided decoding).
  engine.Estimator()->TEST_Refresh();

  // A second, independent batch guarantees the *next* pass is stale: 1000
  // new rows is >10% drift against the 5000-row baseline just built
  // (CardinalityEstimator::kStaleRowFraction == 0.1), so the measured
  // Reconcile() below cannot skip Rebuild() as fresh_enough either.
  for (int i = 5000; i < 6000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       GenerateAttrs(rng, schema, workload, db.reference()),
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  ResetSABIBinCacheStats();
  std::shared_ptr<const GlobalStats> before = engine.Estimator()->Stats();
  engine.Estimator()->TEST_Refresh();  // full stats rebuild over every SST
  // Positive control: a changed snapshot pointer proves a real Rebuild() ran
  // in the measured window (Reconcile() only republishes cached_ when it
  // does not skip as fresh_enough), so the bitmaps_loaded assertion below
  // means what it says instead of passing vacuously on a skipped rebuild.
  EXPECT_NE(before.get(), engine.Estimator()->Stats().get())
      << "stats snapshot didn't change -- TEST_Refresh() skipped the "
         "rebuild (fresh_enough), so bitmaps_loaded == 0 would prove nothing";
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
// Measured empirically: the two floors don't move together, so a schema
// engineered to make ONE bin large relative to what a single-row-match query
// needs to verify opens a wide, reliable window. The query survives not
// because "only a handful of data blocks are needed" is inherently safe at
// this capacity, but because of how strict-capacity LRU decides refusal: an
// insert is refused only when it still doesn't fit after evicting everything
// currently unpinned. The ~4 KB data blocks touched to verify the one match
// fit individually and evict each other as the scan moves on, so they always
// succeed regardless of how many are touched in sequence; the ~28 KB bin can
// never fit under any eviction, full stop.
// 200,000 rows on one ORDERED attr at the coarsest rho (0.5, few/big bins)
// puts one outlier value in a large bin (~28 KB observed) while an EQUAL
// query on that value verifies on the order of 100+ data blocks (succeeds
// down to ~9 KB observed); 16 KB sits in the middle of that window. Full-scan
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
  // 100+ data blocks fetched to confirm the one match.
  QueryCondition cond;
  cond.attr_idx = 0;
  cond.op = CompareOp::EQUAL;
  cond.value = kOutlierValue;
  BitLSMQuery one_match(std::vector<QueryCondition>{cond});
  ASSERT_TRUE(db.VerifyQuery(one_match));

  // Decode-on-repeat alone doesn't prove refusal: an inserted-then-evicted
  // bin (evicted by later data-block inserts during the scan) would also
  // re-decode on an identical repeat. inserts_refused > 0 is the actual
  // refusal proof; the repeat-query decode check instead proves the
  // on-demand path stays live -- Bin() keeps re-reading and re-decoding on
  // the owned-fallback path rather than quietly going stale.
  ResetSABIBinCacheStats();
  ASSERT_TRUE(db.VerifyQuery(one_match));
  SABIBinCacheStats repeat = GetSABIBinCacheStats();
  EXPECT_GT(repeat.inserts_refused, 0u)
      << "no insert was refused -- the bin fit in the cache after all, so "
         "this test no longer exercises the refused-insert fallback";
  EXPECT_GT(repeat.bitmaps_loaded, 0u)
      << "repeat query decoded nothing -- the on-demand path went stale";
}
