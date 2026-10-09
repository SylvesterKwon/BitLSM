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
// per-SST SABIReader histograms and value counts. The memtable is excluded.

// Coordinate axis of one kRange attr over its live [min, max] byte strings,
// after PostgreSQL's convert_string_to_scalar. The common prefix of min and
// max is skipped; each window byte is a digit in the base spanned by the byte
// values the boundaries use, so the coordinate is uniform over the characters
// that occur. For numeric okeys the window is the okey scaled by a power of
// two, so ratios are bit-identical to okey arithmetic. `unit` is one value
// step.
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
// grid over the byte axis, kept as a prefix sum. Mass is a density over the
// coordinate line, so point mass on a queried boundary is smeared, never
// lost.
struct GlobalRangeStats {
  ByteAxis axis;
  std::string min;  // exact global bounds, SABI byte domain
  std::string max;
  std::vector<double> cell_psum;  // cell_psum[i] = mass of cells [0, i]
  double total = 0;               // binned-row mass (NULLs excluded)
  // NDV lower bound: max per-SST distinct count, 0 = unknown. A point's mass
  // is floored at total/ndv so sparse domains do not smear point mass into
  // value holes; a lower bound can only overestimate.
  uint64_t ndv = 0;

  // Per-SST prune-bin geometry for the candidate model: bin-granular pruning
  // admits the matching mass rounded out to bin boundaries, and both the bin
  // mass and the span are per-SST facts.
  struct SSTBins {
    uint64_t lo = 0;  // attr span in this SST, axis coordinates (exact bounds)
    uint64_t hi = 0;
    double binmass = 0;  // binned rows / prune-bin count (equi-depth)
    double total = 0;    // binned rows of this attr in this SST
  };
  std::vector<SSTBins> sst_bins;

  // A ByteInterval placed on the axis as half-open [lo, hi). `point`: spans
  // at most one value step; `in_span`: the lower end lies inside [min, max].
  // Empty windows have hi <= lo.
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
  // Expected candidate mass for the window: per covering SST, the matching
  // share rounded out to prune-bin boundaries, floored at one bin and capped
  // at the SST's binned rows. match_mass is the caller's matching-mass
  // estimate for the same window.
  double CandidateMass(const Window& w, double match_mass) const;

 private:
  // Estimated mass with coordinate strictly below `rel`.
  double CumBelow(uint64_t rel) const;
};

// One kEquality attr: value -> row count merged across live SSTs. Beyond
// CardinalityEstimator::kMaxTrackedValues only the top values are kept and
// `truncated` is set; `total` keeps the full mass.
struct GlobalEqualityStats {
  std::unordered_map<std::string, double> value_counts;
  double total = 0;
  bool truncated = false;
  // Sum over live SSTs of the average bin mass: an equality fetches its
  // value's whole bin in every SST, so this is the expected candidate mass
  // of a point lookup. Assumes the value occurs in every SST.
  double binmass_sum = 0;
};

struct GlobalStats {
  // Indexed by attr; only the slot matching the attr's index type is used,
  // and it stays empty when no live SST has binned rows.
  std::vector<std::optional<GlobalRangeStats>> range;
  std::vector<std::optional<GlobalEqualityStats>> equality;
  // Live SST data entries minus tombstones, shadowing uncorrected.
  uint64_t physical_rows = 0;
  uint64_t live_sst_count = 0;
};

// What EstimateSelectivity returns:
//   output rows      = selectivity * the caller's logical row count
//   expected fetches = candidate_selectivity * physical_rows + memtable_entries
// Fetches exceed output rows because pruning is bin-granular and the memtable
// is unfiltered.
struct EstimateResult {
  // Fraction of live-SST rows expected to match, combined across attrs under
  // independence. Floored so selectivity * physical_rows >= 1.
  double selectivity = 1.0;
  // Live-SST data entries minus tombstone markers.
  uint64_t physical_rows = 0;
  // Attrs whose predicates could not be estimated (no live SSTs, no stats,
  // value dropped by NDV truncation). Their factor is 1.0 in both
  // selectivities; the caller applies its own fallback.
  std::vector<uint32_t> fallback_attrs;
  // Fraction of live-SST rows the read path is expected to fetch:
  // selectivity rounded out to prune-bin boundaries, so
  // candidate_selectivity >= selectivity. Same combine and fallback rules as
  // selectivity.
  double candidate_selectivity = 1.0;
  // Unflushed memtable entries at estimate time; the read path fetches every
  // one of them. Read live per call. 0 without an estimator.
  uint64_t memtable_entries = 0;
};

// Owns the GlobalStats of one column family. A background worker, signalled
// by flush and compaction completion, rebuilds when at least
// kStaleRowFraction of the rows changed, at most once per
// estimator_min_rebuild_interval_ms. Queries never rebuild.
class CardinalityEstimator {
 public:
  static constexpr size_t kMaxTrackedValues =
      10000;  // value-dictionary NDV cap
  // Rebuild only when this fraction of rows sits in files born or dead
  // since the last rebuild.
  static constexpr double kStaleRowFraction = 0.1;

  // Starts the worker and primes an initial build of the live set.
  CardinalityEstimator(rocksdb::DBImpl* db_impl, rocksdb::ColumnFamilyData* cfd,
                       SABISchema schema, const BitLSMOptions& options);
  // Joins the worker. Must run before the DB closes (the worker references
  // the column family).
  ~CardinalityEstimator();

  // The answer when no estimator (or no stats) is available: selectivity 1,
  // physical_rows 0, every queried attr flagged as fallback.
  static EstimateResult FallbackResult(const SABIQuery& q);

  // Current snapshot; never null, empty before the first build.
  std::shared_ptr<const GlobalStats> Stats();

  // Wakes the refresh worker (called from the RocksDB event listener).
  void NotifyChange();

  // Test-only: force one reconcile pass and wait for it, independent of
  // listener timing.
  void TEST_Refresh();

  // Arithmetic over cached stats only. Same-attr conditions intersect into
  // one window, OR clauses use a union bound capped at 1, attrs combine
  // under independence.
  EstimateResult Estimate(const SABIQuery& q);

 private:
  void WorkerLoop();
  // Live unflushed entry count, via the thread-local SuperVersion.
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

// Forwards flush/compaction completion to the column family's estimator.
// Registered before DB::Open; estimators arm and disarm per cf id. Events for
// an unarmed cf are dropped.
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
  // mu_ spans the NotifyChange() call, so once Disarm returns the estimator
  // is safe to destroy.
  void Notify(uint32_t cf_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = targets_.find(cf_id);
    if (it != targets_.end()) it->second->NotifyChange();
  }
  std::mutex mu_;
  std::map<uint32_t, CardinalityEstimator*> targets_;
};

}  // namespace bit_lsm
