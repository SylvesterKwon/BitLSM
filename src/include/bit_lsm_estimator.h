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
// per-SST SABI accessors (SABIReader::OrderedHistogram / -ValueCounts).
// A pure function of the live SST set, so estimates never drift across
// compaction churn. The memtable is excluded: before the first flush every
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
  // Attr NDV lower bound: max per-SST exact distinct count (v6 blobs; 0 =
  // unknown -> floor disabled). Equality floor: a point's mass is at least
  // total/ndv. Sparse integer domains (yyyymm-style value holes) otherwise
  // smear point mass into the holes; a lower-bound NDV can only raise the
  // floor above truth = overestimate = the conservative direction.
  uint64_t ndv = 0;

  // Per-SST prune-bin geometry for the candidate (FPR) model: what the read
  // path's bin-granular pruning admits is the matching mass rounded OUT to
  // bin boundaries, and both the rounding unit (an equi-depth bin's mass)
  // and the reach (the okey span) are per-SST facts. 24 bytes per SST per
  // attr; kept flat so planning stays an O(live SSTs) arithmetic scan.
  struct SSTBins {
    uint64_t lo = 0;     // attr okey span in this SST (exact data bounds)
    uint64_t hi = 0;
    double binmass = 0;  // binned rows / prune-bin count (equi-depth)
    double total = 0;    // binned rows of this attr in this SST
  };
  std::vector<SSTBins> sst_bins;

  // Estimated mass with okey in [lo, hi], both inclusive.
  double RangeMass(uint64_t lo, uint64_t hi) const;
  // RangeMass with the NDV equality floor applied to POINT windows.
  double PointAwareRangeMass(uint64_t lo, uint64_t hi) const;
  // Expected CANDIDATE mass for the window: per covering SST, the matching
  // share rounded out to prune-bin boundaries -- whole bin for a point,
  // half a bin per free (unclipped) edge for a range -- floored at one bin,
  // capped at the SST's binned rows. match_mass is the caller's global
  // matching-mass estimate for the same window (apportioned span-uniformly).
  double CandidateMass(uint64_t lo, uint64_t hi, double match_mass) const;

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
  // Candidate (FPR) scalar: sum over live SSTs of that SST's average bin
  // mass (binned rows / bin count). An equality candidates its value's
  // whole (balance-packed) bin in every SST, so this sum is the expected
  // fetch mass of a point lookup. Unlike ORDERED there is no span to
  // exclude an SST by, so the sum conservatively assumes the value occurs
  // everywhere (an SST without the value prunes to zero at execution).
  double binmass_sum = 0;
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

// What EstimateSelectivity returns, one number per consumer slot:
//   output rows (row slot)  = selectivity * the caller's own logical count
//   expected fetches (cost) = candidate_selectivity * physical_rows
//                             + memtable_entries
// The slots differ because the read path fetches bin-rounded CANDIDATES
// (matching mass rounded out to prune-bin boundaries) plus the whole
// unfiltered memtable, while the plan's output is the matching rows.
// physical_rows is the live-SST sum, shadowing uncorrected — correcting by
// a live/physical ratio is the caller's job.
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
  // (e.g. sysvar constants) for exactly these attrs (BOTH slots).
  std::vector<uint32_t> fallback_attrs;
  // Fraction of physical live-SST rows the read path is expected to FETCH:
  // bin-granular pruning admits matching rows rounded out to prune-bin
  // boundaries, so this is selectivity's bin-rounded counterpart (equality:
  // ~one bin per covering SST even when one row matches). Same independence
  // combine and fallback contract as `selectivity`; invariant
  // candidate_selectivity >= selectivity. Cost slot: expected SST fetches =
  // candidate_selectivity * physical_rows (+ memtable_entries).
  double candidate_selectivity = 1.0;
  // Unflushed (active + immutable memtable) entries at estimate time. No
  // SABI exists before flush, so the read path candidates every one of
  // them regardless of the predicate; the consumer ADDS this to the SST
  // fetch count. Read live per call, never from the rebuilt snapshot
  // (physical_rows deliberately excludes the memtable; this is its
  // complement). 0 without an estimator.
  uint64_t memtable_entries = 0;
};

// Owns the GlobalStats cache for one column family, refreshed by a
// background worker: flush/compaction completion signals the worker, which
// diffs the live file set (in-memory metadata only) and rebuilds when at
// least kStaleRowFraction of the rows changed, at most once per
// estimator_min_rebuild_interval_ms. Queries never rebuild: Stats() is a
// snapshot-pointer copy, and estimates may lag by up to the drift threshold
// plus the memtable.
class CardinalityEstimator {
 public:
  static constexpr size_t kMaxTrackedValues =
      10000;  // value-dictionary NDV cap
  // Rebuild only when this fraction of rows sits in files born or dead
  // since the last rebuild.
  static constexpr double kStaleRowFraction = 0.1;

  // Starts the refresh worker and primes an initial build of the current
  // live set (a reopened DB sees no flush event).
  CardinalityEstimator(rocksdb::DBImpl* db_impl, rocksdb::ColumnFamilyData* cfd,
                       SABISchema schema, const BitLSMOptions& options);
  // Joins the worker. Must run before the DB closes (the worker references
  // the column family).
  ~CardinalityEstimator();

  // The answer when no estimator (or no stats) is available: selectivity 1,
  // physical_rows 0, every queried attr flagged as fallback.
  static EstimateResult FallbackResult(const SABIQuery& q);

  // Current stats snapshot: a pointer copy, never null (empty before the
  // first build), immutable and lock-free to read.
  std::shared_ptr<const GlobalStats> Stats();

  // Wakes the refresh worker (called from the RocksDB event listener).
  void NotifyChange();

  // Test-only: force one reconcile pass and wait for it, independent of
  // listener timing.
  void TEST_Refresh();

  // Cached-stats arithmetic only, no bitmap or row scans. Same-attr
  // conditions intersect into one okey window (BETWEEN-shaped CNF is not
  // squared); UNORDERED equality reads the value dictionary; OR clauses use
  // a union bound capped at 1; attrs combine as an independence product.
  EstimateResult Estimate(const SABIQuery& q);

 private:
  void WorkerLoop();
  // Live unflushed entry count (active + immutable memtables), via the
  // read path's thread-local SuperVersion ref -- hot-path cheap.
  uint64_t MemtableEntries();
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
