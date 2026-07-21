#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_estimator.h"
#include "test_util/bitlsm_test_base.h"

using namespace bit_lsm;

namespace {

// {ORDERED i64, UNORDERED}; rho 0.1 -> 20 bins budget per SST.
BitLSMOptions EstOptions() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/false),
                  AttrSpec{AttrRole::UNORDERED}};
  o.read_seqno = 0;
  o.rho = 0.1;
  return o;
}

void FlushDB(BitLSM& db) {
  ASSERT_TRUE(db.GetInternalDB()->Flush(rocksdb::FlushOptions()).ok());
}

void CompactAllDB(BitLSM& db) {
  rocksdb::CompactRangeOptions cro;
  ASSERT_TRUE(db.GetInternalDB()->CompactRange(cro, nullptr, nullptr).ok());
}

}  // namespace

// Workload: 1000 rows (attr0 = 0..999, attr1 = "x") split over two SSTs at
//           the value-500 boundary; query the aggregated stats.
// Threat: a grid that merges per-SST equi-depth bins wrong (mass lost or
//         double-counted, boundaries misprojected) skews every downstream
//         range selectivity; C5-style okey-magnitude collapse would fold the
//         narrow 0..999 span into one cell.
TEST_F(BitLSMTestBase, OrderedRangeMassMatchesUniformData) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("x")}, "p").ok());
    if (i == 499) FlushDB(db);
  }
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator().Get();
  EXPECT_EQ(stats->physical_rows, 1000u);
  EXPECT_EQ(stats->live_sst_count, 2u);

  ASSERT_TRUE(stats->ordered[0].has_value());
  const GlobalOrderedStats& ord = *stats->ordered[0];
  EXPECT_EQ(ord.min_okey, I64ToOkey(0));
  EXPECT_EQ(ord.max_okey, I64ToOkey(999));
  EXPECT_NEAR(ord.total, 1000.0, 1e-6);
  EXPECT_NEAR(ord.RangeMass(I64ToOkey(0), I64ToOkey(999)), 1000.0, 1e-6);
  EXPECT_NEAR(ord.RangeMass(I64ToOkey(0), I64ToOkey(499)), 500.0, 40.0);
  EXPECT_NEAR(ord.RangeMass(I64ToOkey(750), I64ToOkey(999)), 250.0, 40.0);

  ASSERT_TRUE(stats->unordered[1].has_value());
  const GlobalUnorderedStats& uno = *stats->unordered[1];
  EXPECT_DOUBLE_EQ(uno.total, 1000.0);
  ASSERT_EQ(uno.value_counts.count("x"), 1u);
  EXPECT_DOUBLE_EQ(uno.value_counts.at("x"), 1000.0);
  EXPECT_FALSE(uno.truncated);
}

// Workload: repeated Get() calls with no write activity in between, then one
//           more flush.
// Threat: rebuilding on every planning query blows the ~100us budget; NOT
//         rebuilding after the live SST set changed serves stale stats to
//         every subsequent plan.
TEST_F(BitLSMTestBase, CachesUntilLiveSetChanges) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> s1 = db.Estimator().Get();
  std::shared_ptr<const GlobalStats> s2 = db.Estimator().Get();
  EXPECT_EQ(s1.get(), s2.get()) << "unchanged live set must be a cache hit";
  EXPECT_EQ(s1->physical_rows, 100u);

  for (int64_t i = 100; i < 200; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> s3 = db.Estimator().Get();
  EXPECT_NE(s1.get(), s3.get()) << "flush must invalidate the cached stats";
  EXPECT_EQ(s3->physical_rows, 200u);
}

// Workload: 1000 rows striped over four SSTs (each covers the full 0..999
//           span), estimated before and after full compaction reshapes the
//           LSM into a single run.
// Threat: aggregate error that depends on the SST shape accumulates over
//         compaction churn; the live-set purity constraint requires pre/post
//         estimates to agree within projection error, and totals to agree
//         up to float rounding (mass conservation).
TEST_F(BitLSMTestBase, ChurnKeepsEstimatesStable) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t stripe = 0; stripe < 4; ++stripe) {
    for (int64_t v = stripe; v < 1000; v += 4)
      ASSERT_TRUE(
          db.Put("k" + std::to_string(v), {v, std::string("a")}, "p").ok());
    FlushDB(db);
  }

  std::shared_ptr<const GlobalStats> pre = db.Estimator().Get();
  ASSERT_TRUE(pre->ordered[0].has_value());
  EXPECT_NEAR(pre->ordered[0]->total, 1000.0, 1e-6);
  EXPECT_NEAR(pre->ordered[0]->RangeMass(I64ToOkey(0), I64ToOkey(249)), 250.0,
              35.0);

  CompactAllDB(db);
  std::shared_ptr<const GlobalStats> post = db.Estimator().Get();
  ASSERT_TRUE(post->ordered[0].has_value());
  EXPECT_NEAR(post->ordered[0]->total, 1000.0, 1e-6);
  EXPECT_EQ(post->physical_rows, pre->physical_rows);
  EXPECT_NEAR(post->ordered[0]->RangeMass(I64ToOkey(0), I64ToOkey(249)), 250.0,
              35.0);
}

// Workload: the same unordered value ("red") appearing in two SSTs, plus a
//           value ("blue") in only one.
// Threat: per-SST value counts overwriting instead of summing during the
//         merge understates any value that spans SSTs.
TEST_F(BitLSMTestBase, UnorderedCountsMergeAcrossSSTs) {
  BitLSM& db = OpenDB(EstOptions());
  int64_t key = 0;
  for (int i = 0; i < 100; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key, std::string("red")}, "p").ok());
  FlushDB(db);
  for (int i = 0; i < 50; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key, std::string("red")}, "p").ok());
  for (int i = 0; i < 30; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key, std::string("blue")}, "p")
            .ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator().Get();
  ASSERT_TRUE(stats->unordered[1].has_value());
  const GlobalUnorderedStats& uno = *stats->unordered[1];
  EXPECT_DOUBLE_EQ(uno.value_counts.at("red"), 150.0);
  EXPECT_DOUBLE_EQ(uno.value_counts.at("blue"), 30.0);
  EXPECT_DOUBLE_EQ(uno.total, 180.0);
}

// Workload: every row carries the same ordered value (42) across two SSTs,
//           so the attr's live okey span is a single point.
// Threat: a zero-width grid domain divides by zero or spreads the point mass
//         across cells, corrupting both the equality estimate at 42 and the
//         "nothing above 42" answer.
TEST_F(BitLSMTestBase, SingleValueAttrKeepsPointMass) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 200; ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {int64_t{42}, std::string("a")}, "p")
            .ok());
    if (i == 99) FlushDB(db);
  }
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator().Get();
  ASSERT_TRUE(stats->ordered[0].has_value());
  const GlobalOrderedStats& ord = *stats->ordered[0];
  EXPECT_EQ(ord.min_okey, I64ToOkey(42));
  EXPECT_EQ(ord.max_okey, I64ToOkey(42));
  EXPECT_DOUBLE_EQ(ord.RangeMass(I64ToOkey(42), I64ToOkey(42)), 200.0);
  EXPECT_DOUBLE_EQ(ord.RangeMass(I64ToOkey(43), I64ToOkey(1000)), 0.0);
  EXPECT_DOUBLE_EQ(ord.RangeMass(I64ToOkey(-10), I64ToOkey(41)), 0.0);
}

// Workload: 100 puts in SST1, 20 tombstones for them in SST2, then 20
//           re-puts in SST3.
// Threat: physical_rows feeds the cost slot (expected multi_get count);
//         counting tombstone markers as rows, or dropping shadowed old
//         versions (which the read path still fetches), miscosts every plan.
TEST_F(BitLSMTestBase, PhysicalRowsCountEntriesNotMarkers) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);
  for (int64_t i = 0; i < 20; ++i)
    ASSERT_TRUE(db.Delete("k" + std::to_string(i)).ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator().Get();
  EXPECT_EQ(stats->physical_rows, 100u)
      << "tombstone markers are not fetched rows";

  for (int64_t i = 0; i < 20; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  stats = db.Estimator().Get();
  EXPECT_EQ(stats->physical_rows, 120u)
      << "shadowed old versions stay physical until compacted away";
}

// Workload: rows written but never flushed (memtable only).
// Threat: D-E3 excludes the memtable, so the pre-first-flush state must be
//         visibly empty (callers emit a fallback flag) rather than crash or
//         fabricate stats.
TEST_F(BitLSMTestBase, EmptyBeforeFirstFlush) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());

  std::shared_ptr<const GlobalStats> stats = db.Estimator().Get();
  EXPECT_EQ(stats->physical_rows, 0u);
  EXPECT_EQ(stats->live_sst_count, 0u);
  ASSERT_EQ(stats->ordered.size(), 2u);
  EXPECT_FALSE(stats->ordered[0].has_value());
  EXPECT_FALSE(stats->unordered[1].has_value());
}

// Workload: more distinct unordered values than kMaxTrackedValues in the
//           live set.
// Threat: an unbounded value->count dictionary grows with NDV and turns the
//         planning cache into a memory hog; the cap must demote to top-k and
//         say so instead of silently dropping mass.
TEST_F(BitLSMTestBase, NdvCapDemotesToTopK) {
  BitLSM& db = OpenDB(EstOptions());
  const int64_t ndv =
      static_cast<int64_t>(CardinalityEstimator::kMaxTrackedValues) + 1;
  for (int64_t i = 0; i < ndv; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, "v" + std::to_string(i)}, "p")
            .ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator().Get();
  ASSERT_TRUE(stats->unordered[1].has_value());
  const GlobalUnorderedStats& uno = *stats->unordered[1];
  EXPECT_TRUE(uno.truncated);
  EXPECT_EQ(uno.value_counts.size(), CardinalityEstimator::kMaxTrackedValues);
  EXPECT_NEAR(uno.total, static_cast<double>(ndv), 1.0)
      << "total keeps the full mass, including untracked values";
}
