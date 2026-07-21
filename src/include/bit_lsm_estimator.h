#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_query.h"  // SABIQuery
#include "rocksdb/listener.h"

namespace rocksdb {
class DBImpl;
class ColumnFamilyData;
struct SuperVersion;
}  // namespace rocksdb

namespace bit_lsm {

// Global per-attr statistics over the live SST set, aggregated from the
// per-SST SABI accessors (SABIReader::OrderedHistogram / -ValueCounts) at
// rebuild time. A pure function of the live SST set: projection error is per
// SST and dies with it, so estimates never drift across compaction churn.
// The memtable is excluded by design (D-E3): before the first flush every
// attr slot is empty and physical_rows is 0.

// One ORDERED attr: per-SST equi-depth histograms projected onto a uniform
// okey grid over the live [min_okey, max_okey] span, kept as a prefix sum.
// Mass is treated as a density over the continuous okey line, so point mass
// sitting exactly on a queried boundary is smeared, never lost.
struct GlobalOrderedStats {
  uint64_t min_okey = 0;
  uint64_t max_okey = 0;
  std::vector<double> cell_psum;  // cell_psum[i] = mass of cells [0, i]
  double total = 0;               // binned-row mass (NULLs excluded)

  // Estimated mass with okey in [lo, hi], both inclusive.
  double RangeMass(uint64_t lo, uint64_t hi) const;

 private:
  // Estimated mass with okey strictly below `okey`.
  double CumBelow(uint64_t okey) const;
};

// One UNORDERED attr: value -> row-count dictionary merged across live SSTs.
// Per-value counts inherit the per-SST exactness contract (exact for values
// alone in their bin, uniform-split otherwise). When the merged NDV exceeds
// CardinalityEstimator::kMaxTrackedValues only the top-k values are kept and
// `truncated` is set; `total` always keeps the full mass.
struct GlobalUnorderedStats {
  std::unordered_map<std::string, double> value_counts;
  double total = 0;
  bool truncated = false;
};

struct GlobalStats {
  // Indexed by attr; only the slot matching the attr's role is ever engaged,
  // and it stays empty when no live SST has binned rows for the attr.
  std::vector<std::optional<GlobalOrderedStats>> ordered;
  std::vector<std::optional<GlobalUnorderedStats>> unordered;
  // Live SST data entries minus tombstone markers, shadowing uncorrected:
  // exactly the candidate count the read path would fetch (cost slot).
  uint64_t physical_rows = 0;
  uint64_t live_sst_count = 0;
};

// What EstimateSelectivity returns. The pair (selectivity, physical_rows)
// serves both consumer slots: expected fetches (cost) = selectivity *
// physical_rows, expected output rows = selectivity * the caller's own
// logical row count. physical_rows is the live-SST sum, shadowing
// uncorrected — correcting by a live/physical ratio is the caller's job.
struct EstimateResult {
  // Fraction of physical live-SST rows expected to match the query,
  // combined across attrs under the independence assumption. Floored so
  // selectivity * physical_rows >= 1: stats have blind spots (memtable,
  // bounded staleness), so absence is never reported as exactly 0.
  double selectivity = 1.0;
  // Live-SST data entries minus tombstone markers.
  uint64_t physical_rows = 0;
  // Attrs whose predicates could not be estimated (no live SSTs yet, attr
  // without stats, equality on a value dropped by NDV truncation). Their
  // factor in `selectivity` is 1.0; the caller applies its own fallback
  // (e.g. sysvar constants) for exactly these attrs.
  std::vector<uint32_t> fallback_attrs;
};

// Owns the GlobalStats cache for one column family, refreshed by a small
// background worker: flush/compaction completion (StatsRefreshListener)
// signals the worker, which diffs the live file set against the last
// rebuild's (in-memory metadata only, no table opens) and rebuilds only when
// at least kStaleRowFraction of the rows changed — InnoDB-style auto-recalc,
// paced by estimator_min_rebuild_interval_ms so churn storms (bulk load,
// compaction cascades) coalesce into one pass per interval. Queries never
// rebuild: Stats() is a snapshot-pointer copy, so estimates may lag by up to
// the drift threshold (plus the memtable, D-E3) — acceptable for cost
// estimation by design.
class CardinalityEstimator {
 public:
  static constexpr size_t kMaxTrackedValues = 10000;  // D-E4 NDV cap
  // Rebuild only when this fraction of rows sits in files born or dead
  // since the last rebuild.
  static constexpr double kStaleRowFraction = 0.1;

  // Grid resolution and rebuild pacing come from BitLSMOptions
  // (estimator_grid_cells, estimator_min_rebuild_interval_ms). Starts the
  // refresh worker and primes it with an initial build of the current live
  // set (covers reopen, where no flush event ever fires).
  CardinalityEstimator(rocksdb::DBImpl* db_impl, rocksdb::ColumnFamilyData* cfd,
                       SABISchema schema, const BitLSMOptions& options);
  // Joins the worker. Must run before the DB closes (the worker references
  // the column family).
  ~CardinalityEstimator();

  // The answer when no estimator (or no stats) is available: selectivity 1,
  // physical_rows 0, every queried attr flagged as fallback.
  static EstimateResult FallbackResult(const SABIQuery& q);

  // Current stats snapshot: a pointer copy, never a rebuild, never null
  // (empty stats before the first build). Immutable and safe to read
  // without any lock.
  std::shared_ptr<const GlobalStats> Stats();

  // Wakes the refresh worker (called from the RocksDB event listener).
  void NotifyChange();

  // Test-only: force one reconcile pass and wait for it, independent of
  // listener timing.
  void TEST_Refresh();

  // Planning hot path: cached-stats lookup plus arithmetic only, no bitmap
  // or row scans. ORDERED conditions from single-condition clauses intersect
  // into one per-attr okey window (so BETWEEN-shaped CNF is not squared);
  // UNORDERED equality reads the value dictionary; multi-condition (OR)
  // clauses use a union bound capped at 1; attrs combine as an independence
  // product.
  EstimateResult Estimate(const SABIQuery& q);

 private:
  void WorkerLoop();
  // One pass: diff the live file set, rebuild + publish if drift crossed
  // the threshold. Worker-thread only.
  void Reconcile();
  std::shared_ptr<const GlobalStats> Rebuild(rocksdb::SuperVersion* sv);

  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyData* cfd_;
  SABISchema schema_;
  uint32_t grid_cells_;
  uint32_t min_rebuild_interval_ms_;

  std::mutex mu_;  // guards cached_
  std::shared_ptr<const GlobalStats> cached_;

  // Worker-only state (no lock needed beyond the worker itself).
  std::unordered_map<uint64_t, uint64_t> built_files_;  // file# -> entries
  uint64_t built_entries_ = 0;

  std::mutex worker_mu_;  // guards the signal/stop state below
  std::condition_variable worker_cv_;
  bool stop_ = false;
  uint64_t signal_gen_ = 0;
  uint64_t processed_gen_ = 0;
  std::thread worker_;
};

// Forwards RocksDB flush/compaction completion to the estimator's refresh
// worker. Registered before DB::Open and armed once the estimator exists;
// events while disarmed are dropped.
class StatsRefreshListener : public rocksdb::EventListener {
 public:
  void Arm(CardinalityEstimator* estimator) { target_.store(estimator); }
  void Disarm() { target_.store(nullptr); }
  void OnFlushCompleted(rocksdb::DB*, const rocksdb::FlushJobInfo&) override {
    Notify();
  }
  void OnCompactionCompleted(rocksdb::DB*,
                             const rocksdb::CompactionJobInfo&) override {
    Notify();
  }

 private:
  void Notify() {
    if (CardinalityEstimator* estimator = target_.load()) {
      estimator->NotifyChange();
    }
  }
  std::atomic<CardinalityEstimator*> target_{nullptr};
};

}  // namespace bit_lsm
