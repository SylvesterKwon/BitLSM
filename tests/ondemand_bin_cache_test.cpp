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

// A strict-capacity cache too small for TinyCacheStaysCorrect's one oversized
// bin, but large enough for the data blocks its query verifies against.
rocksdb::BlockBasedTableOptions TinyStrictCacheOptions() {
  rocksdb::BlockBasedTableOptions table_options;
  table_options.block_cache =
      rocksdb::NewLRUCache(16384, /*num_shard_bits=*/0,
                           /*strict_capacity_limit=*/true);
  return table_options;
}

}  // namespace

TEST_F(BitLSMTestBase, SecondQueryServesBinsFromCache) {
  // Seed 1 draws a non-empty-clause query, so the cold run loads bins.
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

// A range predicate's bins are contiguous in the blob, so a cold load
// should issue fewer reads than it decodes bins.
TEST_F(BitLSMTestBase, RangeQueryCoalescesBinReads) {
  Rng rng(3);
  BitLSMOptions schema;
  schema.attr_num = 1;
  schema.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)};
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
  // Positive control: the range decoded several bins, so the coalescing
  // bound cannot hold vacuously.
  ASSERT_GT(cold.bitmaps_loaded, 2u)
      << "range spanned too few bins to exercise coalescing";
  // Count both load paths: on a liburing build a run may arrive through a
  // span-prefetch buffer instead of a pread.
  EXPECT_LT(cold.reads + cold.spans_prefetched, cold.bitmaps_loaded)
      << "cold range query issued one load per bin: no coalescing happened";

  ASSERT_TRUE(db.VerifyQuery(range_query));
  SABIBinCacheStats warm = GetSABIBinCacheStats();
  EXPECT_EQ(warm.reads, cold.reads)
      << "warm repeat issued new file reads instead of hitting the cache";
  EXPECT_EQ(warm.spans_prefetched, cold.spans_prefetched)
      << "warm repeat pulled runs through prefetch buffers again";
  EXPECT_EQ(warm.bitmaps_loaded, cold.bitmaps_loaded)
      << "warm repeat re-decoded bins";
  EXPECT_GT(warm.hits, cold.hits);
}

// Miss runs of different attrs cannot be coalesced, so the span prefetch
// must plan them. Also runs on builds without liburing, where every
// submit is refused.
TEST_F(BitLSMTestBase, MultiAttrColdRunsPlanSpanPrefetch) {
  Rng rng(4);
  BitLSMOptions schema;
  schema.attr_num = 2;
  schema.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
                       AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)};
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
  // region, so no byte-contiguous merge is possible between them.
  QueryCondition c0, c1;
  c0.attr_idx = 0;
  c0.op = CompareOp::GREATER_EQUAL;
  c0.value = 10.0;
  c1.attr_idx = 1;
  c1.op = CompareOp::GREATER_EQUAL;
  c1.value = 50.0;
  BitLSMQuery query(std::vector<QueryCondition>{c0, c1});

  ResetSABIBinCacheStats();
  // Clears the process-wide async-unavailable flag another test may have
  // tripped.
  ResetBlockPrefetchQueueStats();
  ASSERT_TRUE(db.VerifyQuery(query));
  SABIBinCacheStats cold = GetSABIBinCacheStats();
  // The plan engaged on every build: one run per attr. The tombstone bin is
  // absent from the plan since the table has no deletes.
  EXPECT_GE(cold.spans_planned, 2u)
      << "multi-attr cold query did not engage the span-prefetch plan";
  // Each of the two cold loads happens exactly once, by pread or out of a
  // prefetch buffer.
  EXPECT_EQ(cold.reads + cold.spans_prefetched, 2u);
#ifdef BITLSM_HAVE_URING
  // Positive control where CMake found liburing: the async branch must
  // actually run.
  ASSERT_FALSE(GetBlockPrefetchQueueStats().async_unavailable)
      << "liburing build refused async reads";
#endif
  if (GetBlockPrefetchQueueStats().async_unavailable) {
    // No liburing: every planned run was dropped and took the sync pread.
    EXPECT_EQ(cold.spans_prefetched, 0u);
    EXPECT_EQ(cold.spans_dropped, cold.spans_planned)
        << "runs neither submitted nor counted as dropped";
  } else {
    EXPECT_GT(cold.spans_prefetched, 0u)
        << "async reads available but no run was served from a buffer";
    EXPECT_EQ(cold.spans_dropped, 0u)
        << "runs were dropped despite working async reads";
  }

  // Warm repeat: every bin is cached, so nothing is read again on either
  // path.
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

// A stats rebuild must count rows from the cardinality directory, not by
// decoding bins.
TEST_F(BitLSMTestBase, EstimatorRebuildLoadsNoBitmaps) {
  Rng rng(1);
  BitLSMOptions schema = GenerateSchema(rng);
  schema.ondemand_index = true;
  schema.enable_estimator = true;
  BitLSM& engine = OpenDB(schema);
  CheckedBitLSM db(&engine, schema);
  WorkloadParams workload;
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       GenerateAttrs(rng, schema, workload, db.reference()),
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());
  // Drain the flush listener's own rebuild first, so the measured refresh
  // below cannot be skipped as fresh enough.
  engine.Estimator()->TEST_Refresh();

  // A second batch of 1000 rows is >10% drift against the 5000-row baseline,
  // so the next pass must rebuild.
  for (int i = 5000; i < 6000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       GenerateAttrs(rng, schema, workload, db.reference()),
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  ResetSABIBinCacheStats();
  std::shared_ptr<const GlobalStats> before = engine.Estimator()->Stats();
  engine.Estimator()->TEST_Refresh();
  // Positive control: a changed snapshot pointer proves a rebuild ran.
  EXPECT_NE(before.get(), engine.Estimator()->Stats().get())
      << "stats snapshot unchanged; TEST_Refresh() skipped the rebuild";
  SABIBinCacheStats after = GetSABIBinCacheStats();
  EXPECT_EQ(after.bitmaps_loaded, 0u)
      << "stats rebuild decoded bitmaps; use the persisted cardinalities";
}

// A cache too small for one bin; refused inserts fall back to
// iterator-owned bins.
//
// The block cache also backs RocksDB's data blocks, and a refused data-block
// insert is a hard iterator error. So the schema makes one bin (~28 KB) far
// larger than any data block (~4 KB): a strict-capacity LRU refuses an insert
// only when it cannot fit after evicting everything unpinned, so the data
// blocks keep fitting one at a time while the bin never does. A full scan is
// not exercised here since its cache floor scales with row count.
TEST_F(BitLSMTestBase, TinyCacheStaysCorrect) {
  Rng rng(2);
  BitLSMOptions schema;
  schema.attr_num = 1;
  schema.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8)};
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

  // Matches exactly the outlier row; the bin covering it is refused, so the
  // fallback view has to be right.
  QueryCondition cond;
  cond.attr_idx = 0;
  cond.op = CompareOp::EQUAL;
  cond.value = kOutlierValue;
  BitLSMQuery one_match(std::vector<QueryCondition>{cond});
  ASSERT_TRUE(db.VerifyQuery(one_match));

  // inserts_refused > 0 proves the refusal; the repeat-query decode proves
  // the owned-fallback path keeps re-reading rather than going stale.
  ResetSABIBinCacheStats();
  ASSERT_TRUE(db.VerifyQuery(one_match));
  SABIBinCacheStats repeat = GetSABIBinCacheStats();
  EXPECT_GT(repeat.inserts_refused, 0u)
      << "no insert was refused; the bin fit in the cache";
  EXPECT_GT(repeat.bitmaps_loaded, 0u) << "repeat query decoded nothing";
}
