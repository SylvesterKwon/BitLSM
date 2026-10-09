#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

// {kRange i64, kEquality}; rho 0.1 -> 20 bins budget per SST.
BitLSMOptions EstOptions() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kInt, 8),
                  AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  o.read_seqno = 0;
  o.rho = 0.1;
  o.enable_estimator = true;
  o.estimator_min_rebuild_interval_ms = 0;  // deterministic tests
  return o;
}

// Flush/compact and force one synchronous stats reconcile, so tests observe
// deterministic freshness regardless of listener timing.
// Closed okey interval [a, b] placed on the attr's axis.
GlobalRangeStats::Window Between(const GlobalRangeStats& ord, int64_t a,
                                 int64_t b) {
  ByteInterval w =
      ByteInterval::FromOp(CompareOp::GREATER_EQUAL, OkeyToBytes(I64ToOkey(a)));
  w.Intersect(
      ByteInterval::FromOp(CompareOp::LESS_EQUAL, OkeyToBytes(I64ToOkey(b))));
  return ord.Place(w);
}

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

// 1000 rows 0..999 split over two SSTs at 500; the merged grid must
// reproduce the uniform distribution.
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

  ASSERT_TRUE(stats->range[0].has_value());
  const GlobalRangeStats& ord = *stats->range[0];
  EXPECT_EQ(ord.min, OkeyToBytes(I64ToOkey(0)));
  EXPECT_EQ(ord.max, OkeyToBytes(I64ToOkey(999)));
  EXPECT_NEAR(ord.total, 1000.0, 1e-6);
  EXPECT_NEAR(ord.RangeMass(Between(ord, 0, 999)), 1000.0, 1e-6);
  EXPECT_NEAR(ord.RangeMass(Between(ord, 0, 499)), 500.0, 40.0);
  EXPECT_NEAR(ord.RangeMass(Between(ord, 750, 999)), 250.0, 40.0);

  ASSERT_TRUE(stats->equality[1].has_value());
  const GlobalEqualityStats& uno = *stats->equality[1];
  EXPECT_DOUBLE_EQ(uno.total, 1000.0);
  ASSERT_EQ(uno.value_counts.count("x"), 1u);
  EXPECT_DOUBLE_EQ(uno.value_counts.at("x"), 1000.0);
  EXPECT_FALSE(uno.truncated);
}

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

// Estimates before and after a full compaction must agree.
TEST_F(BitLSMTestBase, ChurnKeepsEstimatesStable) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t stripe = 0; stripe < 4; ++stripe) {
    for (int64_t v = stripe; v < 1000; v += 4)
      ASSERT_TRUE(
          db.Put("k" + std::to_string(v), {v, std::string("a")}, "p").ok());
    FlushDB(db);
  }

  std::shared_ptr<const GlobalStats> pre = db.Estimator()->Stats();
  ASSERT_TRUE(pre->range[0].has_value());
  EXPECT_NEAR(pre->range[0]->total, 1000.0, 1e-6);
  EXPECT_NEAR(pre->range[0]->RangeMass(Between(*pre->range[0], 0, 249)), 250.0,
              35.0);

  CompactAllDB(db);
  std::shared_ptr<const GlobalStats> post = db.Estimator()->Stats();
  ASSERT_TRUE(post->range[0].has_value());
  EXPECT_NEAR(post->range[0]->total, 1000.0, 1e-6);
  EXPECT_EQ(post->physical_rows, pre->physical_rows);
  EXPECT_NEAR(post->range[0]->RangeMass(Between(*post->range[0], 0, 249)),
              250.0, 35.0);
}

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
  ASSERT_TRUE(stats->equality[1].has_value());
  const GlobalEqualityStats& uno = *stats->equality[1];
  EXPECT_DOUBLE_EQ(uno.value_counts.at("red"), 150.0);
  EXPECT_DOUBLE_EQ(uno.value_counts.at("blue"), 30.0);
  EXPECT_DOUBLE_EQ(uno.total, 180.0);
}

// Every row has the same value, so the live span is a single point.
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
  ASSERT_TRUE(stats->range[0].has_value());
  const GlobalRangeStats& ord = *stats->range[0];
  EXPECT_EQ(ord.min, OkeyToBytes(I64ToOkey(42)));
  EXPECT_EQ(ord.max, OkeyToBytes(I64ToOkey(42)));
  EXPECT_DOUBLE_EQ(ord.RangeMass(Between(ord, 42, 42)), 200.0);
  EXPECT_DOUBLE_EQ(ord.RangeMass(Between(ord, 43, 1000)), 0.0);
  EXPECT_DOUBLE_EQ(ord.RangeMass(Between(ord, -10, 41)), 0.0);
}

// Tombstone markers are not rows; shadowed old versions still are, since
// the read path fetches them.
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

TEST_F(BitLSMTestBase, EmptyBeforeFirstFlush) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  EXPECT_EQ(stats->physical_rows, 0u);
  EXPECT_EQ(stats->live_sst_count, 0u);
  ASSERT_EQ(stats->range.size(), 2u);
  EXPECT_FALSE(stats->range[0].has_value());
  EXPECT_FALSE(stats->equality[1].has_value());
}

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
  ASSERT_TRUE(stats->equality[1].has_value());
  const GlobalEqualityStats& uno = *stats->equality[1];
  EXPECT_TRUE(uno.truncated);
  EXPECT_EQ(uno.value_counts.size(), CardinalityEstimator::kMaxTrackedValues);
  EXPECT_NEAR(uno.total, static_cast<double>(ndv), 1.0)
      << "total keeps the full mass, including untracked values";
}

namespace {

SABICondition Ord(uint32_t attr, CompareOp op, int64_t v) {
  return SABICondition{attr,
                       ByteInterval::FromOp(op, OkeyToBytes(I64ToOkey(v))), ""};
}

SABICondition Uno(uint32_t attr, const std::string& v) {
  return SABICondition{attr, {}, v};
}

SABICondition Str(uint32_t attr, CompareOp op, const std::string& v) {
  return SABICondition{attr, ByteInterval::FromOp(op, v), ""};
}

double EstimatedRows(const EstimateResult& r) {
  return r.selectivity * static_cast<double>(r.physical_rows);
}

}  // namespace

// a0 >= 0 AND a0 <= 499 as two clauses must estimate 500, not 0.5 * 0.5.
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

// a0 and a1 are independent, so the product should land near 250.
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

// An absent value floors at one row instead of falling back.
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

// red OR blue covers everything; an OR must not multiply like an AND.
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

// EQUAL on a kRange attr folds into a [42, 42] window.
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

// Skewed data; a wide range must keep q-error <= 1.5.
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

// A writer thread flushes while the main thread estimates.
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

// A 5% flush stays under kStaleRowFraction, so the old snapshot is served.
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

// Three 4% flushes; drift accumulates against the last rebuild, not the
// last check.
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

// Seeded random input; the merged value counts must equal a reference
// merge of the written rows.
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
  ASSERT_TRUE(stats->equality[1].has_value());
  const GlobalEqualityStats& uno = *stats->equality[1];
  ASSERT_EQ(uno.value_counts.size(), reference.size());
  double expected_total = 0;
  for (const auto& [value, count] : reference) {
    ASSERT_EQ(uno.value_counts.count(value), 1u) << value;
    EXPECT_NEAR(uno.value_counts.at(value), count, 1e-9) << value;
    expected_total += count;
  }
  EXPECT_NEAR(uno.total, expected_total, 1e-9);
}

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

TEST_F(BitLSMTestBase, GridCellsOptionControlsResolution) {
  BitLSMOptions opt = EstOptions();
  opt.estimator_grid_cells = 64;
  BitLSM& db = OpenDB(opt);
  for (int64_t i = 0; i < 500; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());
  FlushDB(db);

  std::shared_ptr<const GlobalStats> stats = db.Estimator()->Stats();
  ASSERT_TRUE(stats->range[0].has_value());
  EXPECT_EQ(stats->range[0]->cell_psum.size(), 64u);
  EXPECT_NEAR(stats->range[0]->total, 500.0, 1e-6);
}

// Every other test refreshes through TEST_Refresh; this one relies on the
// flush listener alone.
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

// No flush event fires after a reopen, so the open-time build has to
// populate the stats.
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

// ---- Candidate slot and memtable term: the read path fetches bin-rounded
// candidates plus every unflushed entry, not matching rows ----

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

// One matching row still fetches a whole bin, ~total/B.
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

// A range with both edges inside the SST overshoots by half a bin per
// edge.
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

// Two SSTs with disjoint spans and different sizes; a point query must be
// priced by the covering SST's bin mass, not a global average.
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
      << "point in the 1500-row SST costs that SST's bin (1500/10)";

  SABIQuery q_small;
  q_small.clause_groups = {{Ord(0, CompareOp::EQUAL, 1700)}};
  EXPECT_NEAR(CandidateRows(db.EstimateSelectivity(q_small)), 50.0, 1.0)
      << "point in the 500-row SST costs only 500/10";
}

// The dictionary knows the exact match (20), but the read path fetches
// the whole shared bin (~200).
TEST_F(BitLSMTestBase, CandidateUnorderedSharedBinMass) {
  BitLSM& db = OpenDB(EstOptions());
  FillBinShared2000(db);

  SABIQuery q;
  q.clause_groups = {{Uno(1, "v37")}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_NEAR(EstimatedRows(r), 20.0, 1.0);
  EXPECT_NEAR(CandidateRows(r), 200.0, 1.0);
}

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

// One value holds 95% of the rows, far above the average bin mass.
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

// Three points in three bins; the OR must sum bins, not matches.
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

// Unflushed entries have no SABI, so the read path fetches all of them.
TEST_F(BitLSMTestBase, MemtableEntriesSurfaceInEstimate) {
  BitLSM& db = OpenDB(EstOptions());
  for (int64_t i = 0; i < 100; ++i)
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {i, std::string("a")}, "p").ok());

  SABIQuery q;
  q.clause_groups = {{Ord(0, CompareOp::EQUAL, 5)}};
  EstimateResult r = db.EstimateSelectivity(q);
  EXPECT_EQ(r.memtable_entries, 100u)
      << "memtable count missing from the pre-first-flush answer";
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

// yyyymm-shaped integers: 84 values over a 611-wide span. The grid smears
// point mass into the holes; the NDV floor must pull an equality back to
// total/ndv, while ranges stay uncorrected.
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
  ASSERT_TRUE(stats->range[0].has_value());
  const GlobalRangeStats& ord = *stats->range[0];
  EXPECT_EQ(ord.ndv, 84u);
  // The raw grid smears the point into the holes (~840/611 per okey unit).
  EXPECT_LT(ord.RangeMass(Between(ord, 199401, 199401)), 5.0);
  // The floor restores the per-value truth (840/84 = 10).
  EXPECT_NEAR(ord.PointAwareRangeMass(Between(ord, 199401, 199401)), 10.0, 2.0);
  // Whole-domain range: mass conserved, no correction.
  EXPECT_NEAR(ord.RangeMass(Between(ord, 199201, 199812)), 840.0, 1e-6);
}

// ---- Byte axis: kVarBinary attrs estimate like numerics ----

namespace {
// {kRange kVarBinary}; rho 0.1.
BitLSMOptions StrOptions() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kVarBinary)};
  o.read_seqno = 0;
  o.rho = 0.1;
  o.enable_estimator = true;
  o.estimator_min_rebuild_interval_ms = 0;
  return o;
}

std::string Padded(const char* prefix, int i, int digits) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s%0*d", prefix, digits, i);
  return buf;
}
}  // namespace

// Axis over "user-00".."user-99": the common prefix must be skipped and
// out-of-span strings must saturate to the ends.
TEST(ByteAxis, StripsCommonPrefixAndSaturates) {
  ByteAxis axis = ByteAxis::Fit("user-00", "user-99", '0', '9', 7);
  EXPECT_EQ(axis.prefix, "user-");
  EXPECT_EQ(axis.radix, 10u);
  EXPECT_EQ(axis.digits, 19u);
  EXPECT_EQ(axis.Rel("user-00"), 0u);
  EXPECT_EQ(axis.Rel("user-99"), axis.span);
  // Digits only: the coordinate is uniform in the decimal value.
  EXPECT_NEAR(static_cast<double>(axis.Rel("user-50")) / axis.span, 50.0 / 99,
              1e-9);
  EXPECT_LT(axis.Rel("user-49"), axis.Rel("user-5"));
  EXPECT_EQ(axis.Rel("apple"), 0u);
  EXPECT_EQ(axis.Rel("user"), 0u);
  EXPECT_EQ(axis.Rel("zzz"), axis.End());
  EXPECT_EQ(axis.Rel("user-995"), axis.End());
  // One unit steps the last real digit of "user-NN".
  EXPECT_EQ(axis.Rel("user-01") - axis.Rel("user-00"), axis.unit);
}

TEST(ByteAxis, OkeysKeepBase256) {
  ByteAxis axis = ByteAxis::Fit(OkeyToBytes(I64ToOkey(0)),
                                OkeyToBytes(I64ToOkey(999)), 0, 255, 8);
  EXPECT_EQ(axis.radix, 256u);
  EXPECT_EQ(axis.digits, 8u);
  EXPECT_EQ(axis.prefix.size(), 6u);
  EXPECT_EQ(axis.unit, 1ull << 48);
  EXPECT_EQ(axis.Rel(OkeyToBytes(I64ToOkey(999))), 999ull << 48);
  EXPECT_EQ(axis.Rel(OkeyToBytes(I64ToOkey(500))), 500ull << 48);
}

// Range predicates in every shape on a kRange kVarBinary attr must
// estimate within histogram tolerance and never fall back.
TEST_F(BitLSMTestBase, EstimateVarBinaryRangesOnCommonPrefixData) {
  BitLSM& db = OpenDB(StrOptions());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {Padded("prefix-", i, 4)}, "p").ok());
    if (i == 499) FlushDB(db);
  }
  FlushDB(db);

  auto rows = [&](std::vector<SABIOrClause> groups) {
    SABIQuery q;
    q.clause_groups = std::move(groups);
    EstimateResult r = db.EstimateSelectivity(q);
    EXPECT_EQ(r.physical_rows, 1000u);
    EXPECT_TRUE(r.fallback_attrs.empty());
    return EstimatedRows(r);
  };
  EXPECT_NEAR(rows({{Str(0, CompareOp::GREATER_EQUAL, "prefix-0250")},
                    {Str(0, CompareOp::LESS, "prefix-0750")}}),
              500.0, 60.0);
  EXPECT_NEAR(rows({{Str(0, CompareOp::GREATER_EQUAL, "prefix-0900")}}), 100.0,
              30.0);
  EXPECT_NEAR(rows({{Str(0, CompareOp::LESS, "prefix-0100")}}), 100.0, 30.0);
  EXPECT_NEAR(rows({{Str(0, CompareOp::GREATER, "prefix-0899")}}), 100.0, 30.0);
  EXPECT_NEAR(rows({{Str(0, CompareOp::GREATER_EQUAL, "")}}), 1000.0, 1e-6);
}

// A point on a continuous axis has ~0 mass, so the NDV floor answers it;
// bounds outside the span estimate the 1-row floor.
TEST_F(BitLSMTestBase, EstimateVarBinaryPointAndOutOfSpan) {
  BitLSM& db = OpenDB(StrOptions());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(
        db.Put("k" + std::to_string(i), {Padded("prefix-", i, 4)}, "p").ok());
    if (i == 499) FlushDB(db);
  }
  FlushDB(db);

  auto rows = [&](SABICondition c) {
    SABIQuery q;
    q.clause_groups = {{std::move(c)}};
    EstimateResult r = db.EstimateSelectivity(q);
    EXPECT_TRUE(r.fallback_attrs.empty());
    return EstimatedRows(r);
  };
  const double point = rows(Str(0, CompareOp::EQUAL, "prefix-0123"));
  EXPECT_GE(point, 1.0);
  EXPECT_LE(point, 5.0);
  EXPECT_LE(rows(Str(0, CompareOp::LESS, "a")), 1.0 + 1e-9);
  EXPECT_LE(rows(Str(0, CompareOp::GREATER_EQUAL, "zzz")), 1.0 + 1e-9);
  EXPECT_LE(rows(Str(0, CompareOp::GREATER, "prefix-0999")), 1.0 + 1e-9);
  EXPECT_NEAR(rows(Str(0, CompareOp::LESS_EQUAL, "prefix-0999")), 1000.0, 1e-6);
}

TEST_F(BitLSMTestBase, EstimateVarBinaryMixedLengthsStaysBounded) {
  BitLSM& db = OpenDB(StrOptions());
  for (int i = 0; i < 300; ++i) {
    std::string v(1 + (i % 40), static_cast<char>('a' + (i % 26)));
    ASSERT_TRUE(db.Put("k" + std::to_string(i), {v}, "p").ok());
  }
  FlushDB(db);
  for (const char* c : {"", "a", "ab", "m", "mmmmmmmmmmmmmmmmmmmmmmmm", "z"}) {
    for (CompareOp op :
         {CompareOp::EQUAL, CompareOp::LESS, CompareOp::LESS_EQUAL,
          CompareOp::GREATER, CompareOp::GREATER_EQUAL}) {
      SABIQuery q;
      q.clause_groups = {{Str(0, op, c)}};
      EstimateResult r = db.EstimateSelectivity(q);
      EXPECT_TRUE(r.fallback_attrs.empty());
      EXPECT_GE(r.selectivity, 0.0);
      EXPECT_LE(r.selectivity, 1.0);
      EXPECT_GE(r.candidate_selectivity, r.selectivity);
      EXPECT_LE(r.candidate_selectivity, 1.0);
    }
  }
  SABIQuery q;
  q.clause_groups = {{Str(0, CompareOp::GREATER_EQUAL, "m")}};
  EXPECT_NEAR(EstimatedRows(db.EstimateSelectivity(q)), 300.0 * 14 / 26, 60.0);
}
