#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
// per-SST SABI accessors (SABIReader::RangeHistogram / -ValueCounts).
// A pure function of the live SST set, so estimates never drift across
// compaction churn. The memtable is excluded: before the first flush every
// attr slot is empty and physical_rows is 0.

// Coordinate axis of one kRange attr over its live [min, max] byte strings
// (PostgreSQL's convert_string_to_scalar, on bytes). The common prefix of
// min and max orders nothing inside the span, so the window starts after
// it; each window byte is a digit in the base spanned by the byte values the
// boundaries actually use (digits only -> base 10, 19 digits; arbitrary
// bytes -> base 256, 8 digits), so the coordinate is uniform over the
// characters that occur instead of over all 256 byte values. Strings inside
// the window keep their memcmp order; longer ones sharing a window collapse
// onto one coordinate (only estimates depend on it, never pruning). For
// okeys whose low byte spans 0..255 -- every realistic numeric attr -- the
// window is the okey scaled by a power of two, so every ratio the grid and
// the candidate model compute is bit-identical to okey arithmetic. `unit` is
// one value step: exactly one okey for numerics, one step in the last real
// digit of the shortest boundary otherwise.
struct ByteAxis {
  std::string prefix;
  uint8_t lo_char = 0;   // smallest byte value after the prefix
  uint32_t radix = 256;  // hi_char - lo_char + 1, at least 2
  size_t digits = 8;     // window length: radix^digits <= 2^64
  uint64_t base = 0;     // window of min
  uint64_t span = 0;     // window of max, minus base
  uint64_t unit = 1;

  // Axis over [min, max] whose boundaries use bytes in [lo_char, hi_char]
  // after the common prefix; `shortest` is the shortest boundary length.
  static ByteAxis Fit(std::string_view min, std::string_view max,
                      uint8_t lo_char, uint8_t hi_char, size_t shortest);
  // s[k, k+digits) as a base-`radix` number, bytes clamped into the radix
  // and missing trailing bytes read as the lowest digit.
  uint64_t Window(std::string_view s) const;
  // Coordinate of a string inside [min, max]; saturates to 0 / End() when
  // handed something outside.
  uint64_t Rel(std::string_view s) const;
  // One unit past the last coordinate: a half-open end there covers max.
  uint64_t End() const;
};

// One kRange attr: per-SST equi-depth histograms projected onto a uniform
// grid over the attr's byte axis, kept as a prefix sum. Mass is treated as a
// density over the continuous coordinate line, so point mass sitting exactly
// on a queried boundary is smeared, never lost.
struct GlobalRangeStats {
  ByteAxis axis;
  std::string min;  // exact global bounds, SABI byte domain
  std::string max;
  std::vector<double> cell_psum;  // cell_psum[i] = mass of cells [0, i]
  double total = 0;               // binned-row mass (NULLs excluded)
  // Attr NDV lower bound: max per-SST exact distinct count (0 = unknown ->
  // floor disabled). Equality floor: a point's mass is at least total/ndv.
  // Sparse integer domains (yyyymm-style value holes) otherwise smear point
  // mass into the holes; a lower-bound NDV can only raise the floor above
  // truth = overestimate = the conservative direction.
  uint64_t ndv = 0;

  // Per-SST prune-bin geometry for the candidate (FPR) model: what the read
  // path's bin-granular pruning admits is the matching mass rounded OUT to
  // bin boundaries, and both the rounding unit (an equi-depth bin's mass)
  // and the reach (the axis span) are per-SST facts. 24 bytes per SST per
  // attr; kept flat so planning stays an O(live SSTs) arithmetic scan.
  struct SSTBins {
    uint64_t lo = 0;  // attr span in this SST, axis coordinates (exact bounds)
    uint64_t hi = 0;
    double binmass = 0;  // binned rows / prune-bin count (equi-depth)
    double total = 0;    // binned rows of this attr in this SST
  };
  std::vector<SSTBins> sst_bins;

  // A ByteInterval placed on the axis as half-open coordinates [lo, hi).
  // `point`: the window spans at most one value step (x = v, or strict
  // bounds enclosing exactly one value); `in_span`: its lower end lies
  // inside [min, max]. Empty windows come out with hi <= lo.
  struct Window {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool point = false;
    bool in_span = false;
  };
  Window Place(const ByteInterval& w) const;

  // Estimated mass inside the window.
  double RangeMass(const Window& w) const;
  // RangeMass with the NDV equality floor applied to in-span points.
  double PointAwareRangeMass(const Window& w) const;
  // Expected CANDIDATE mass for the window: per covering SST, the matching
  // share rounded out to prune-bin boundaries -- whole bin for a point,
  // half a bin per free (unclipped) edge for a range -- floored at one bin,
  // capped at the SST's binned rows. match_mass is the caller's global
  // matching-mass estimate for the same window (apportioned span-uniformly).
  double CandidateMass(const Window& w, double match_mass) const;

 private:
  // Estimated mass with coordinate strictly below `rel`.
  double CumBelow(uint64_t rel) const;
};

// One kEquality attr: value -> row-count dictionary merged across live SSTs.
// Per-value counts inherit the per-SST exactness contract (exact for values
// alone in their bin, uniform-split otherwise). When the merged NDV exceeds
// CardinalityEstimator::kMaxTrackedValues only the top-k values are kept and
// `truncated` is set; `total` always keeps the full mass.
struct GlobalEqualityStats {
  std::unordered_map<std::string, double> value_counts;
  double total = 0;
  bool truncated = false;
  // Candidate (FPR) scalar: sum over live SSTs of that SST's average bin
  // mass (binned rows / bin count). An equality candidates its value's
  // whole (balance-packed) bin in every SST, so this sum is the expected
  // fetch mass of a point lookup. Unlike kRange there is no span to
  // exclude an SST by, so the sum conservatively assumes the value occurs
  // everywhere (an SST without the value prunes to zero at execution).
  double binmass_sum = 0;
};

struct GlobalStats {
  // Indexed by attr; only the slot matching the attr's index type is ever
  // engaged, and it stays empty when no live SST has binned rows for the attr.
  std::vector<std::optional<GlobalRangeStats>> range;
  std::vector<std::optional<GlobalEqualityStats>> equality;
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
  // conditions intersect into one byte window (BETWEEN-shaped CNF is not
  // squared); kEquality equality reads the value dictionary; OR clauses use
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

// Forwards RocksDB flush/compaction completion to the owning column family's
// estimator. Registered before DB::Open (the listener list is frozen there);
// estimators arm and disarm per cf id as column families come and go. Events
// for an unarmed cf are dropped.
class StatsRefreshListener : public rocksdb::EventListener {
 public:
  void Arm(uint32_t cf_id, CardinalityEstimator* estimator) {
    std::lock_guard<std::mutex> lock(mu_);
    targets_[cf_id] = estimator;
  }
  void Disarm(uint32_t cf_id) {
    std::lock_guard<std::mutex> lock(mu_);
    targets_.erase(cf_id);
  }
  void DisarmAll() {
    std::lock_guard<std::mutex> lock(mu_);
    targets_.clear();
  }
  void OnFlushCompleted(rocksdb::DB*,
                        const rocksdb::FlushJobInfo& info) override {
    Notify(info.cf_id);
  }
  void OnCompactionCompleted(rocksdb::DB*,
                             const rocksdb::CompactionJobInfo& info) override {
    Notify(info.cf_id);
  }

 private:
  // mu_ spans the NotifyChange() call so Disarm/DisarmAll cannot return while
  // a notification for that estimator is still in flight; after they return,
  // the estimator is safe to destroy. NotifyChange never re-enters the
  // listener, so the extended critical section cannot deadlock.
  void Notify(uint32_t cf_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = targets_.find(cf_id);
    if (it != targets_.end()) it->second->NotifyChange();
  }
  std::mutex mu_;
  std::map<uint32_t, CardinalityEstimator*> targets_;
};

}  // namespace bit_lsm
