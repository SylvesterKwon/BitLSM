#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
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
  o.enable_estimator = true;
  o.estimator_min_rebuild_interval_ms = 0;  // deterministic tests
  return o;
}

// Flush/compact and force one synchronous stats reconcile, so tests observe
// deterministic freshness regardless of listener timing.
void FlushDB(BitLSM& db) {
  ASSERT_TRUE(db.GetInternalDB()->Flush(rocksdb::FlushOptions()).ok());
  if (db.Estimator()) db.Estimator()->TEST_Refresh();
}

void CompactAllDB(BitLSM& db) {
  rocksdb::CompactRangeOptions cro;
  ASSERT_TRUE(db.GetInternalDB()->CompactRange(cro, nullptr, nullptr).ok());
  if (db.Estimator()) db.Estimator()->TEST_Refresh();
}

}  // namespace

// Workload: 1000 rows (attr0 = 0..999, attr1 = "x") split over two SSTs at
//           the value-500 boundary; query the aggregated stats.
// Threat: a grid that merges per-SST equi-depth bins wrong (mass lost or
//         double-counted, boundaries misprojected) skews every downstream
//         range selectivity; okey-magnitude precision collapse would fold the
//         narrow 0..999 span into one cell.
TEST_F(BitLSMTestBase, OrderedRangeMassMatchesUniformData) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("x")}, "p").ok());
    if (i == 499) FlushDB(db);
  }
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
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

  std::shared_ptr<const GlobalStats> s1 = db.Estimator()->Stats();
  std::shared_ptr<const GlobalStats> s2 = db.Estimator()->Stats();
  EXPECT_EQ(s1.get(), s2.get()) << "unchanged live set must be a cache hit";
  EXPECT_EQ(s1->physical_rows, 100u);

  for (int64_t i = 100; i < 200; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> s3 = db.Estimator()->Stats();
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

  std::shared_ptr<const GlobalStats> pre = db.Estimator()->Stats();
  ASSERT_TRUE(pre->ordered[0].has_value());
  EXPECT_NEAR(pre->ordered[0]->total, 1000.0, 1e-6);
  EXPECT_NEAR(pre->ordered[0]->RangeMass(I64ToOkey(0), I64ToOkey(249)), 250.0,
              35.0);

  CompactAllDB(db);
  std::shared_ptr<const GlobalStats> post = db.Estimator()->Stats();
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

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
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

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
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

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  EXPECT_EQ(stats->physical_rows, 100u)
      << "tombstone markers are not fetched rows";

  for (int64_t i = 0; i < 20; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  stats = db.Estimator()->Stats();
  EXPECT_EQ(stats->physical_rows, 120u)
      << "shadowed old versions stay physical until compacted away";
}

// Workload: rows written but never flushed (memtable only).
// Threat: the memtable is excluded from stats, so the pre-first-flush state
// must be
//         visibly empty (callers emit a fallback flag) rather than crash or
//         fabricate stats.
TEST_F(BitLSMTestBase, EmptyBeforeFirstFlush) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
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

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  ASSERT_TRUE(stats->unordered[1].has_value());
  const GlobalUnorderedStats& uno = *stats->unordered[1];
  EXPECT_TRUE(uno.truncated);
  EXPECT_EQ(uno.value_counts.size(), CardinalityEstimator::kMaxTrackedValues);
  EXPECT_NEAR(uno.total, static_cast<double>(ndv), 1.0)
      << "total keeps the full mass, including untracked values";
}

namespace {

SABICondition Ord(uint32_t attr, CompareOp op, int64_t v) {
  return SABICondition{attr, op, OkeyInterval::FromOp(op, I64ToOkey(v)), ""};
}

SABICondition Uno(uint32_t attr, const std::string& v) {
  return SABICondition{attr, CompareOp::EQUAL, {}, v};
}

double EstimatedRows(const EstimateResult& r) {
  return r.selectivity * static_cast<double>(r.physical_rows);
}

}  // namespace

// Workload: 1000 rows 0..999 over two SSTs; BETWEEN-shaped query arriving as
//           CNF, i.e. TWO clauses on the same attr (a0 >= 0, a0 <= 499).
// Threat: treating same-attr clauses as independent predicates squares the
//         range selectivity (0.5 * 0.5 -> 250 est instead of 500).
TEST_F(BitLSMTestBase, EstimateIntersectsSameAttrRanges) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("x")}, "p").ok());
    if (i == 499) FlushDB(db);
  }
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::GREATER_EQUAL, 0)},
                     {Ord(0, CompareOp::LESS_EQUAL, 499)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_EQ(r.physical_rows, 1000u);
  EXPECT_TRUE(r.fallback_attrs.empty());
  EXPECT_NEAR(EstimatedRows(r), 500.0, 40.0);
}

// Workload: a0 uniform 0..999 and a1 red/blue alternating (statistically
//           independent), conjunctive query a0 <= 499 AND a1 = "red".
// Threat: the independence-product combine must land near truth (250) for
//         uncorrelated attrs.
TEST_F(BitLSMTestBase, EstimateConjunctionIndependenceProduct) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       {i, std::string(i % 2 ? "red" : "blue")}, "p")
                    .ok());
    if (i == 499) FlushDB(db);
  }
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::LESS_EQUAL, 499)}, {Uno(1, "red")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_TRUE(r.fallback_attrs.empty());
  EXPECT_NEAR(EstimatedRows(r), 250.0, 40.0);
}

// Workload: red x150 / blue x30 across two SSTs; equality on a tracked value
//           and on a value absent from the (exact, untruncated) dictionary.
// Threat: a tracked value must reproduce its merged count; an absent value
//         must floor at one matching row (never exactly 0 — stats have blind
//         spots and 0 is absorbing in cost arithmetic) without falling back
//         to the magic constants.
TEST_F(BitLSMTestBase, EstimateUnorderedEqualityAndAbsentValue) {
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

  SABIQuery q_red;
  q_red.clause_groups = {{Uno(1, "red")}};
  EstimateResult r = db.EstimateSelectivity(q_red);
  EXPECT_TRUE(r.fallback_attrs.empty());
  EXPECT_NEAR(EstimatedRows(r), 150.0, 1e-6);

  SABIQuery q_green;
  q_green.clause_groups = {{Uno(1, "green")}};
  EstimateResult g = db.EstimateSelectivity(q_green);
  EXPECT_TRUE(g.fallback_attrs.empty());
  EXPECT_DOUBLE_EQ(EstimatedRows(g), 1.0);
}

// Workload: one OR clause spanning both unordered values (red OR blue) over
//           the red x150 / blue x30 data.
// Threat: an OR clause folded like an AND (product) collapses the estimate
//         to ~14% instead of 100%; the union bound must cap at full mass.
TEST_F(BitLSMTestBase, EstimateOrClauseUnionBound) {
  BitLSM& db = OpenDB(EstOptions());
  int64_t key = 0;
  for (int i = 0; i < 150; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key, std::string("red")}, "p").ok());
  for (int i = 0; i < 30; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key, std::string("blue")}, "p")
            .ok());
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Uno(1, "red"), Uno(1, "blue")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_TRUE(r.fallback_attrs.empty());
  EXPECT_NEAR(EstimatedRows(r), 180.0, 1e-6);
}

// Workload: rows only in the memtable (no flush yet), then a query touching
//           both attrs.
// Threat: the memtable is excluded from stats, so the pre-first-flush estimate
// is
//         meaningless; the caller must get an explicit per-attr fallback
//         flag, not a silent selectivity over zero rows.
TEST_F(BitLSMTestBase, EstimateEmptyLiveSetFlagsFallback) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::GREATER_EQUAL, 10)}, {Uno(1, "a")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_EQ(r.physical_rows, 0u);
  EXPECT_DOUBLE_EQ(r.selectivity, 1.0);
  EXPECT_EQ(r.fallback_attrs, (std::vector<uint32_t>{0, 1}));
}

// Workload: equality on an ORDERED attr whose live span is the single value
//           42 (EQUAL folds into a degenerate [42,42] window).
// Threat: EQUAL handled as an unbounded window (or the point mass smeared
//         away) breaks equality estimates on ordered attrs.
TEST_F(BitLSMTestBase, EstimateOrderedEqualityPointMass) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 200; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {int64_t{42}, std::string("a")}, "p")
            .ok());
  FlushDB(db);

  SABIQuery q42;
  q42.clause_groups = {{Ord(0, CompareOp::EQUAL, 42)}};
  EXPECT_NEAR(EstimatedRows(db.EstimateSelectivity(q42)), 200.0, 1e-6);

  SABIQuery q43;
  q43.clause_groups = {{Ord(0, CompareOp::EQUAL, 43)}};
  EXPECT_DOUBLE_EQ(EstimatedRows(db.EstimateSelectivity(q43)), 1.0);
}

// Workload: zipf-like skew (value v in 1..100 appears floor(1000/v) times,
//           ~5187 rows) over two SSTs; wide range query a0 <= 10 with true
//           selectivity ~56%.
// Threat: under skew, uniform-within-bin projection
//         plus grid interpolation must keep wide-range q-error <= 1.5.
TEST_F(BitLSMTestBase, EstimateZipfWideRangeQError) {
  BitLSM& db = OpenDB(EstOptions());
  std::vector<int64_t> vals;
  for (int64_t v = 1; v <= 100; ++v)
    for (int64_t c = 0; c < 1000 / v; ++c) vals.push_back(v);

  int64_t truth = 0;
  for (size_t i = 0; i < vals.size(); ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {vals[i], std::string("z")}, "p").ok());
    if (vals[i] <= 10) ++truth;
    if (i == vals.size() / 2) FlushDB(db);
  }
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::LESS_EQUAL, 10)}};
  double est = EstimatedRows(db.EstimateSelectivity(q));
  ASSERT_GT(est, 0.0);
  double q_error = std::max(est / truth, truth / est);
  EXPECT_LE(q_error, 1.5) << "est " << est << " vs truth " << truth;
}

// Workload: a writer thread doing put+flush churn while the main thread
//           hammers EstimateSelectivity.
// Threat: rebuild walks the version storage and table cache while flushes
//         install new SuperVersions; a lifetime race there corrupts stats or
//         crashes planning mid-flight.
TEST_F(BitLSMTestBase, EstimateSafeDuringFlushChurn) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 500; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i % 1000, std::string("a")}, "p")
            .ok());
  FlushDB(db);

  std::atomic<bool> writer_ok{true};
  std::thread writer([&db, &writer_ok] {
    int64_t key = 0;
    for (int round = 0; round < 20; ++round) {
      for (int i = 0; i < 200; ++i, ++key) {
        if (!db.Put("c" + std::to_string(key), {key % 1000, std::string("b")},
                    "p")
                 .ok())
          writer_ok = false;
      }
      if (!db.GetInternalDB()->Flush(rocksdb::FlushOptions()).ok())
        writer_ok = false;
    }
  });

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::LESS_EQUAL, 500)}, {Uno(1, "a")}};
  for (int i = 0; i < 2000; ++i) {
    EstimateResult r = db.EstimateSelectivity(q);
    EXPECT_GE(r.selectivity, 0.0);
    EXPECT_LE(r.selectivity, 1.0);
    EXPECT_GE(r.physical_rows, 500u);
  }
  writer.join();
  EXPECT_TRUE(writer_ok);

  db.Estimator()->TEST_Refresh();
  EstimateResult final_r = db.EstimateSelectivity(q);
  // Staleness bound: unbuilt drift stays under kStaleRowFraction of the
  // built rows, so the served count may lag the true 4500 by at most ~10%.
  EXPECT_GE(final_r.physical_rows, 4091u);
  EXPECT_LE(final_r.physical_rows, 4500u);
}

// Workload: a 10000-row base SST, then one 500-row flush (5% of the built
//           rows).
// Threat: rebuilding on every SuperVersion change ties rebuild frequency to
//         the LSM event rate instead of the data change rate; sub-threshold
//         churn must keep serving the existing snapshot (bounded staleness
//         is the contract).
TEST_F(BitLSMTestBase, BelowThresholdFlushServesExistingStats) {
  rocksdb_options_.disable_auto_compactions = true;
  BitLSM& db = OpenDB(EstOptions());
  int64_t key = 0;
  for (int i = 0; i < 10000; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key % 1000, std::string("a")}, "p")
            .ok());
  FlushDB(db);
  std::shared_ptr<const GlobalStats> s1 = db.Estimator()->Stats();
  EXPECT_EQ(s1->physical_rows, 10000u);

  for (int i = 0; i < 500; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key % 1000, std::string("a")}, "p")
            .ok());
  FlushDB(db);
  std::shared_ptr<const GlobalStats> s2 = db.Estimator()->Stats();
  EXPECT_EQ(s1.get(), s2.get()) << "5% drift must not trigger a rebuild";
  EXPECT_EQ(s2->physical_rows, 10000u);
}

// Workload: a 10000-row base SST, then three 400-row flushes — cumulative
//           drift 4% / 8% / 12% relative to the last rebuild.
// Threat: measuring drift against the last *check* instead of the last
//         *rebuild* resets the counter every flush, so small flushes would
//         never accumulate past the threshold and stats would go stale
//         forever.
TEST_F(BitLSMTestBase, CumulativeDriftTriggersRebuild) {
  rocksdb_options_.disable_auto_compactions = true;
  BitLSM& db = OpenDB(EstOptions());
  int64_t key = 0;
  for (int i = 0; i < 10000; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key % 1000, std::string("a")}, "p")
            .ok());
  FlushDB(db);
  std::shared_ptr<const GlobalStats> s1 = db.Estimator()->Stats();

  for (int flush = 0; flush < 2; ++flush) {
    for (int i = 0; i < 400; ++i, ++key)
      ASSERT_TRUE(
          db.Put("k" + std::to_string(key), {key % 1000, std::string("a")}, "p")
              .ok());
    FlushDB(db);
    EXPECT_EQ(db.Estimator()->Stats().get(), s1.get())
        << "drift " << 4 * (flush + 1) << "% is still below the threshold";
  }

  for (int i = 0; i < 400; ++i, ++key)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(key), {key % 1000, std::string("a")}, "p")
            .ok());
  FlushDB(db);
  std::shared_ptr<const GlobalStats> s2 = db.Estimator()->Stats();
  EXPECT_NE(s1.get(), s2.get()) << "12% cumulative drift must rebuild";
  EXPECT_EQ(s2->physical_rows, 11200u);
}

// Workload: seeded-random multi-SST input — 15 distinct values, 10 flushes,
//           random per-flush row counts; NDV stays under the per-SST bin
//           budget so per-SST counts are exact.
// Threat: the global value merge must equal a plain reference merge of the
//         written rows; any dedup, summing, or ordering bug in the merge
//         shows up as a per-value count mismatch.
TEST_F(BitLSMTestBase, UnorderedMergeMatchesReferenceOnRandomInput) {
  rocksdb_options_.disable_auto_compactions = true;
  BitLSM& db = OpenDB(EstOptions());
  std::mt19937 rng(20260721);
  std::map<std::string, double> reference;
  int64_t key = 0;
  for (int flush = 0; flush < 10; ++flush) {
    int rows = 50 + static_cast<int>(rng() % 150);
    for (int i = 0; i < rows; ++i, ++key) {
      std::string v = "v" + std::to_string(rng() % 15);
      reference[v] += 1.0;
      // Constant ordered attr: its 1-bin cardinality leaves the whole bin
      // budget to the unordered attr, keeping per-SST counts exact.
      ASSERT_TRUE(db.Put("k" + std::to_string(key), {int64_t{7}, v}, "p").ok());
    }
    FlushDB(db);
  }
  // Oversized final flush: guaranteed to cross the drift threshold, so the
  // last rebuild captures the complete live set.
  for (int i = 0; i < 500; ++i, ++key) {
    std::string v = "v" + std::to_string(rng() % 15);
    reference[v] += 1.0;
    ASSERT_TRUE(db.Put("k" + std::to_string(key), {int64_t{7}, v}, "p").ok());
  }
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  ASSERT_TRUE(stats->unordered[1].has_value());
  const GlobalUnorderedStats& uno = *stats->unordered[1];
  ASSERT_EQ(uno.value_counts.size(), reference.size());
  double expected_total = 0;
  for (const auto& [value, count] : reference) {
    ASSERT_EQ(uno.value_counts.count(value), 1u) << value;
    EXPECT_NEAR(uno.value_counts.at(value), count, 1e-9) << value;
    expected_total += count;
  }
  EXPECT_NEAR(uno.total, expected_total, 1e-9);
}

// Workload: options that never enable the estimator (the default), then an
//           estimate call against flushed data.
// Threat: standalone users must pay nothing for the estimator and get a
//         graceful all-fallback answer, not a crash on a missing component.
TEST_F(BitLSMTestBase, EstimatorDisabledByDefault) {
  BitLSMOptions opt = EstOptions();
  opt.enable_estimator = false;
  BitLSM& db = OpenDB(opt);
  EXPECT_EQ(db.Estimator(), nullptr);
  for (int64_t i = 0; i < 10; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::GREATER_EQUAL, 0)}, {Uno(1, "a")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_DOUBLE_EQ(r.selectivity, 1.0);
  EXPECT_EQ(r.physical_rows, 0u);
  EXPECT_EQ(r.fallback_attrs, (std::vector<uint32_t>{0, 1}));
}

// Workload: estimator enabled with a non-default 64-cell grid.
// Threat: the option must actually reach the aggregation (a silently ignored
//         knob would invalidate every grid-resolution experiment).
TEST_F(BitLSMTestBase, GridCellsOptionControlsResolution) {
  BitLSMOptions opt = EstOptions();
  opt.estimator_grid_cells = 64;
  BitLSM& db = OpenDB(opt);
  for (int64_t i = 0; i < 500; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  ASSERT_TRUE(stats->ordered[0].has_value());
  EXPECT_EQ(stats->ordered[0]->cell_psum.size(), 64u);
  EXPECT_NEAR(stats->ordered[0]->total, 500.0, 1e-6);
}

// Workload: 100 rows flushed with a raw Flush (no test-side refresh), then
//           the stats are polled without a single estimate call.
// Threat: the push design's whole point is that stats refresh without
//         queries; a dead listener wiring would be masked everywhere else by
//         the tests' explicit TEST_Refresh calls.
TEST_F(BitLSMTestBase, ListenerRefreshesWithoutQueries) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  ASSERT_TRUE(db.GetInternalDB()->Flush(rocksdb::FlushOptions()).ok());

  uint64_t physical = 0;
  for (int i = 0; i < 5000 && physical != 100; ++i) {
    physical = db.Estimator()->Stats()->physical_rows;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(physical, 100u);
}

// Workload: write and flush 100 rows, close the DB, reopen it, and read the
//           stats without any write or estimate activity.
// Threat: after a reopen no flush event ever fires, so without an open-time
//         initial build the stats would stay empty (all-fallback) until the
//         first churn.
TEST_F(BitLSMTestBase, InitialBuildRunsAtReopen) {
  BitLSMOptions opt = EstOptions();
  {
    BitLSM& db = OpenDB(opt);
    for (int64_t i = 0; i < 100; ++i)
      ASSERT_TRUE(
          db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
    FlushDB(db);
  }
  BitLSM& db2 = OpenDB(opt);
  db2.Estimator()->TEST_Refresh();
  EXPECT_EQ(db2.Estimator()->Stats()->physical_rows, 100u);
}

// ---------------------------------------------------------------------------
// Candidate (FPR) slot + memtable term: the cost consumer prices FETCHES, and
// the read path fetches bin-rounded candidates plus every unflushed entry,
// not matching rows. candidate_selectivity models the former,
// memtable_entries the latter. (MTR-scale validation: 21-point residual
// table, 2026-07-22.)
// ---------------------------------------------------------------------------

namespace {

double CandidateRows(const EstimateResult& r) {
  return r.candidate_selectivity * static_cast<double>(r.physical_rows);
}

// 2000 rows in one SST: a0 = 0..1999 unique, a1 = 100 distinct values x20
// rows. rho 0.1 x 2 attrs -> 20-bin budget, uniform water-fill -> B=[10,10],
// so ordered bin mass = unordered bin mass = 2000/10 = 200 exactly.
void FillBinShared2000(BitLSM& db) {
  for (int64_t i = 0; i < 2000; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, "v" + std::to_string(i % 100)}, "p")
            .ok());
  FlushDB(db);
}

}  // namespace

// Workload: equality on a high-NDV ordered attr (NDV 2000 >> 10 bins), one
//           matching row.
// Threat: pricing fetches by MATCHING mass understates the whole-bin
//         candidate cost by ~bin_mass/match (the SF2 444x misselection); an
//         equality must cost one full equi-depth bin, ~total/B.
TEST_F(BitLSMTestBase, CandidateEqualityCostsWholeBin) {
  BitLSM& db = OpenDB(EstOptions());
  FillBinShared2000(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::EQUAL, 1000)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_TRUE(r.fallback_attrs.empty());
  EXPECT_NEAR(EstimatedRows(r), 1.0, 0.5) << "row slot stays matching mass";
  EXPECT_NEAR(CandidateRows(r), 200.0, 1.0) << "cost slot pays one bin";
  EXPECT_GE(r.candidate_selectivity, r.selectivity);
}

// Workload: interior 400-row range on the same high-NDV attr (both window
//           edges free, i.e. strictly inside the SST span).
// Threat: bin-boundary overshoot beyond the matching mass must be priced --
//         one half-bin per free edge in expectation -- without ballooning a
//         range whose mass already dwarfs the smear.
TEST_F(BitLSMTestBase, CandidateRangeAddsEdgeSmear) {
  BitLSM& db = OpenDB(EstOptions());
  FillBinShared2000(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::GREATER_EQUAL, 500)},
                     {Ord(0, CompareOp::LESS_EQUAL, 899)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_NEAR(EstimatedRows(r), 400.0, 20.0);
  EXPECT_NEAR(CandidateRows(r), 600.0, 30.0)
      << "match 400 + 2 free edges x half a 200-row bin";
}

// Workload: two SSTs with disjoint okey spans and 3x different sizes (1500
//           rows over 0..1499, 500 rows over 1500..1999), point queries into
//           each span.
// Threat: one global bin-mass scalar prices both points identically (200);
//         the real fetch cost is the covering SST's own bin mass (150 vs
//         50) -- per-SST span awareness is load-bearing (time-ordered ingest
//         makes disjoint spans the common LSM shape).
TEST_F(BitLSMTestBase, CandidateFollowsSpanAcrossSSTs) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 1500; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, "v" + std::to_string(i % 100)}, "p")
            .ok());
  FlushDB(db);
  for (int64_t i = 1500; i < 2000; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, "v" + std::to_string(i % 100)}, "p")
            .ok());
  FlushDB(db);

  SABIQuery q_big;
  q_big.clause_groups = {{Ord(0, CompareOp::EQUAL, 700)}};
  EXPECT_NEAR(CandidateRows(db.EstimateSelectivity(q_big)), 150.0, 1.0)
      << "point in the 1500-row SST pays that SST's bin (1500/10)";

  SABIQuery q_small;
  q_small.clause_groups = {{Ord(0, CompareOp::EQUAL, 1700)}};
  EXPECT_NEAR(CandidateRows(db.EstimateSelectivity(q_small)), 50.0, 1.0)
      << "point in the 500-row SST pays only 500/10";
}

// Workload: equality on an unordered value sharing its bin with 9 others
//           (100 distinct values, 10 bins, balanced packing).
// Threat: the dictionary answers the MATCH exactly (20), but the read path
//         candidates the whole shared bin (~200); the cost slot must pay the
//         bin, not the dictionary count.
TEST_F(BitLSMTestBase, CandidateUnorderedSharedBinMass) {
  BitLSM& db = OpenDB(EstOptions());
  FillBinShared2000(db);

  SABIQuery q;
  q.clause_groups = {{Uno(1, "v37")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_NEAR(EstimatedRows(r), 20.0, 1.0);
  EXPECT_NEAR(CandidateRows(r), 200.0, 1.0);
}

// Workload: conjunction of the 400-row range and the shared-bin unordered
//           equality (independent by construction).
// Threat: candidate fractions must compose as an independence product like
//         match fractions do -- the bitmap AND intersects candidates, which
//         is exactly why conjunctive queries cancel FPR and stay bi-friendly.
TEST_F(BitLSMTestBase, CandidateConjunctionMultipliesFractions) {
  BitLSM& db = OpenDB(EstOptions());
  FillBinShared2000(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::GREATER_EQUAL, 500)},
                     {Ord(0, CompareOp::LESS_EQUAL, 899)},
                     {Uno(1, "v37")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_NEAR(EstimatedRows(r), 4.0, 1.5) << "0.2 x 0.01 x 2000";
  EXPECT_NEAR(CandidateRows(r), 60.0, 8.0) << "0.3 x 0.1 x 2000";
}

// Workload: one unordered value carrying 95% of the rows (alone in its bin
//           under balanced packing), equality on it.
// Threat: candidates are a superset of matches, so candidate_selectivity <
//         selectivity is a contradiction the consumer would turn into
//         fetch_count < output_rows; the invariant must hold under skew
//         where the avg bin mass (200) sits far below the match (1900).
TEST_F(BitLSMTestBase, CandidateNeverBelowMatch) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 1900; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("hot")}, "p").ok());
  for (int64_t i = 1900; i < 2000; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, "t" + std::to_string(i)}, "p")
            .ok());
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Uno(1, "hot")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_NEAR(EstimatedRows(r), 1900.0, 40.0);
  EXPECT_GE(r.candidate_selectivity, r.selectivity);
  EXPECT_NEAR(CandidateRows(r), 1900.0, 40.0)
      << "matches floor the candidates when they exceed the avg bin";
}

// Workload: an IN-shaped OR clause of three ordered points in three distinct
//           bins.
// Threat: each point candidates its own whole bin; summing member matches
//         (~3) instead of member bins (~600) reintroduces the equality
//         underpricing through the OR door.
TEST_F(BitLSMTestBase, CandidateOrClauseSumsMemberBins) {
  BitLSM& db = OpenDB(EstOptions());
  FillBinShared2000(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::EQUAL, 100),
                      Ord(0, CompareOp::EQUAL, 1000),
                      Ord(0, CompareOp::EQUAL, 1900)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_NEAR(EstimatedRows(r), 3.0, 1.5);
  EXPECT_NEAR(CandidateRows(r), 600.0, 5.0);
}

// Workload: unflushed rows only, then a flush, then more unflushed rows --
//           estimates taken at each stage.
// Threat: unflushed entries have no SABI, so the read path candidates every
//         one of them regardless of the predicate (MTR phase C: a predicate
//         missing the SST span entirely still fetched exactly the memtable
//         count). A consumer without this term underprices every fresh
//         table; a stale count would misprice right after a flush.
TEST_F(BitLSMTestBase, MemtableEntriesSurfaceInEstimate) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::EQUAL, 5)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_EQ(r.memtable_entries, 100u)
      << "pre-first-flush: the memtable count must ride the fallback answer";
  EXPECT_EQ(r.physical_rows, 0u);

  FlushDB(db);
  r = db.EstimateSelectivity(q);
  EXPECT_EQ(r.memtable_entries, 0u) << "flush drains the term to zero";
  EXPECT_EQ(r.physical_rows, 100u);

  for (int64_t i = 100; i < 150; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  r = db.EstimateSelectivity(q);
  EXPECT_EQ(r.memtable_entries, 50u) << "live count, not rebuild-time stale";
}

// Workload: estimator disabled (standalone default), estimate after a flush.
// Threat: the new fields must keep neutral defaults on the no-estimator
//         path -- candidate factor 1 and no memtable term -- so existing
//         fallback consumers see no behavior change.
TEST_F(BitLSMTestBase, CandidateFieldsNeutralWhenDisabled) {
  BitLSMOptions opt = EstOptions();
  opt.enable_estimator = false;
  BitLSM& db = OpenDB(opt);
  for (int64_t i = 0; i < 10; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::EQUAL, 5)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_DOUBLE_EQ(r.candidate_selectivity, 1.0);
  EXPECT_EQ(r.memtable_entries, 0u);
}

// Workload: yyyymm-shaped sparse integers -- 84 real values (7 years x 12
//           months) over a 611-wide okey span with 89-value holes at every
//           year boundary; 10 rows per value.
// Threat: the continuous-density grid smears point mass into the holes, so
//         an equality reads ~total/span instead of ~total/ndv (the SF2 q1_2
//         11x underestimate). The v6 NDV floor must pull it back to the
//         per-value truth; RANGE estimates integrate over the holes and must
//         stay uncorrected.
TEST_F(BitLSMTestBase, SparseDomainEqualityNdvFloor) {
  BitLSM& db = OpenDB(EstOptions());
  int rows = 0;
  for (int y = 1992; y <= 1998; ++y)
    for (int m = 1; m <= 12; ++m)
      for (int r = 0; r < 10; ++r)
        ASSERT_TRUE(db.Put("k" + std::to_string(rows++),
                           {int64_t(y * 100 + m), std::string("x")}, "p")
                        .ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  ASSERT_TRUE(stats->ordered[0].has_value());
  const GlobalOrderedStats& ord = *stats->ordered[0];
  EXPECT_EQ(ord.ndv, 84u);
  // The raw grid smears the point into the holes (~840/611 per okey unit).
  EXPECT_LT(ord.RangeMass(I64ToOkey(199401), I64ToOkey(199401)), 5.0);
  // The floor restores the per-value truth (840/84 = 10).
  EXPECT_NEAR(ord.PointAwareRangeMass(I64ToOkey(199401), I64ToOkey(199401)),
              10.0, 2.0);
  // Whole-domain range: mass conserved, no correction.
  EXPECT_NEAR(ord.RangeMass(I64ToOkey(199201), I64ToOkey(199812)), 840.0, 1e-6);
}
