#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bit_lsm_encoding.h"

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

// Owns the lazily rebuilt GlobalStats cache for one column family. Get() is
// the planning hot path: a SuperVersion-number comparison plus a shared_ptr
// copy on a cache hit. The SST set only changes when a new SuperVersion is
// installed (flush/compaction), so that number doubles as the dirty flag —
// no write-path hook needed.
class CardinalityEstimator {
 public:
  static constexpr uint32_t kGridCells = 256;         // D-E2
  static constexpr size_t kMaxTrackedValues = 10000;  // D-E4 NDV cap

  CardinalityEstimator(rocksdb::DBImpl* db_impl, rocksdb::ColumnFamilyData* cfd,
                       SABISchema schema);

  // Returns the stats for the current live SST set, rebuilding first when
  // the SuperVersion changed since the cached build. Thread-safe; the
  // returned snapshot is immutable and safe to use without the DB lock.
  std::shared_ptr<const GlobalStats> Get();

 private:
  std::shared_ptr<const GlobalStats> Rebuild(rocksdb::SuperVersion* sv);

  rocksdb::DBImpl* db_impl_;
  rocksdb::ColumnFamilyData* cfd_;
  SABISchema schema_;

  std::mutex mu_;
  uint64_t built_sv_number_ = 0;  // 0 = never built
  std::shared_ptr<const GlobalStats> cached_;
};

}  // namespace bit_lsm
