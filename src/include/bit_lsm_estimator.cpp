#include "bit_lsm_estimator.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

using namespace std;
using namespace rocksdb;

namespace bit_lsm {

double GlobalOrderedStats::CumBelow(uint64_t okey) const {
  if (okey <= min_okey || total == 0) return 0;
  uint64_t span = max_okey - min_okey;
  uint64_t s = okey - min_okey;
  if (span == 0 || s > span) return total;
  // Position on the [0, cells] axis, computed on the min-shifted span so
  // narrow spans at large okey magnitudes keep double precision.
  double p = static_cast<double>(s) / static_cast<double>(span) *
             static_cast<double>(cell_psum.size());
  uint32_t cell = std::min(static_cast<uint32_t>(p),
                           static_cast<uint32_t>(cell_psum.size() - 1));
  double below = cell == 0 ? 0 : cell_psum[cell - 1];
  double cell_mass = cell_psum[cell] - below;
  return below + cell_mass * (p - cell);
}

double GlobalOrderedStats::RangeMass(uint64_t lo, uint64_t hi) const {
  if (hi < lo || total == 0) return 0;
  // Inclusive upper edge: okeys are integers, so "<= hi" is "< hi + 1".
  double upper = hi >= max_okey ? total : CumBelow(hi + 1);
  return std::max(0.0, upper - CumBelow(lo));
}

double GlobalOrderedStats::PointAwareRangeMass(uint64_t lo, uint64_t hi) const {
  double mass = RangeMass(lo, hi);
  // NDV equality floor, point windows INSIDE the live span only: the grid
  // smears mass uniformly over the okey span, so on a sparse domain an
  // existing point reads ~total/span_used instead of ~total/ndv. Points
  // OUTSIDE [min_okey, max_okey] keep their zero -- there it is proof of
  // absence, not smear. (In-span holes do get floored: NDV alone cannot
  // tell a hole from a value, and overestimating a hole is the conservative
  // direction.) Range windows integrate over the holes and need no
  // correction (verified exact on SF2 BETWEENs).
  if (lo == hi && ndv > 0 && lo >= min_okey && lo <= max_okey)
    mass = std::max(mass, total / double(ndv));
  return std::min(mass, total);
}

namespace {

// Projects one per-SST histogram onto the uniform grid over [min_okey,
// max_okey]: each source bin's count is spread over the cells it overlaps,
// proportional to overlap length (uniform-within-bin assumption), in
// min_okey-shifted coordinates.
void ProjectHistogram(const OrderedAttrHistogram& hist, uint64_t min_okey,
                      uint64_t max_okey, vector<double>& cells) {
  uint64_t span = max_okey - min_okey;
  const size_t n = cells.size();
  if (span == 0) {
    for (uint64_t c : hist.counts) cells[0] += static_cast<double>(c);
    return;
  }
  double cell_w = static_cast<double>(span) / static_cast<double>(n);
  for (size_t b = 0; b + 1 < hist.boundaries.size(); ++b) {
    double count = static_cast<double>(hist.counts[b]);
    if (count == 0) continue;
    double s = static_cast<double>(hist.boundaries[b] - min_okey);
    double e = static_cast<double>(hist.boundaries[b + 1] - min_okey);
    if (e <= s) {  // zero-width bin: all mass at one point
      size_t cell = std::min(static_cast<size_t>(s / cell_w), n - 1);
      cells[cell] += count;
      continue;
    }
    size_t c0 = std::min(static_cast<size_t>(s / cell_w), n - 1);
    size_t c1 = std::min(static_cast<size_t>(e / cell_w), n - 1);
    for (size_t c = c0; c <= c1; ++c) {
      double cl = c * cell_w;
      double cr = cl + cell_w;
      double overlap = std::min(e, cr) - std::max(s, cl);
      if (overlap > 0) cells[c] += count * (overlap / (e - s));
    }
  }
}

}  // namespace

CardinalityEstimator::CardinalityEstimator(DBImpl* db_impl,
                                           ColumnFamilyData* cfd,
                                           SABISchema schema,
                                           const BitLSMOptions& options)
    : db_impl_(db_impl),
      cfd_(cfd),
      schema_(std::move(schema)),
      grid_cells_(std::max(1u, options.estimator_grid_cells)),
      min_rebuild_interval_ms_(options.estimator_min_rebuild_interval_ms) {
  auto empty = std::make_shared<GlobalStats>();
  empty->ordered.resize(schema_.attr_num());
  empty->unordered.resize(schema_.attr_num());
  cached_ = std::move(empty);
  worker_ = std::thread([this] { WorkerLoop(); });
  NotifyChange();  // prime: build the current live set at open
}

CardinalityEstimator::~CardinalityEstimator() {
  {
    lock_guard<mutex> lock(worker_mu_);
    stop_ = true;
  }
  worker_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

EstimateResult CardinalityEstimator::FallbackResult(const SABIQuery& q) {
  EstimateResult res;
  std::set<uint32_t> attrs;
  for (const auto& clause : q.clause_groups)
    for (const auto& cond : clause) attrs.insert(cond.attr_idx);
  res.fallback_attrs.assign(attrs.begin(), attrs.end());
  return res;
}

std::shared_ptr<const GlobalStats> CardinalityEstimator::Stats() {
  lock_guard<mutex> lock(mu_);
  return cached_;
}

void CardinalityEstimator::NotifyChange() {
  {
    lock_guard<mutex> lock(worker_mu_);
    ++signal_gen_;
  }
  worker_cv_.notify_all();
}

void CardinalityEstimator::TEST_Refresh() {
  uint64_t target;
  {
    lock_guard<mutex> lock(worker_mu_);
    target = ++signal_gen_;
  }
  worker_cv_.notify_all();
  std::unique_lock<std::mutex> lock(worker_mu_);
  worker_cv_.wait(lock, [&] { return stop_ || processed_gen_ >= target; });
}

void CardinalityEstimator::WorkerLoop() {
  auto last_reconcile = std::chrono::steady_clock::time_point::min();
  std::unique_lock<std::mutex> lock(worker_mu_);
  while (true) {
    worker_cv_.wait(lock,
                    [this] { return stop_ || signal_gen_ > processed_gen_; });
    if (stop_) return;
    // Pace the passes: a churn storm's signals coalesce into one reconcile
    // per interval.
    auto not_before =
        last_reconcile + std::chrono::milliseconds(min_rebuild_interval_ms_);
    while (!stop_ && std::chrono::steady_clock::now() < not_before)
      worker_cv_.wait_until(lock, not_before);
    if (stop_) return;
    uint64_t target = signal_gen_;
    lock.unlock();
    Reconcile();
    last_reconcile = std::chrono::steady_clock::now();
    lock.lock();
    processed_gen_ = target;
    worker_cv_.notify_all();  // wake TEST_Refresh waiters
  }
}

void CardinalityEstimator::Reconcile() {
  SuperVersion* sv = cfd_->GetReferencedSuperVersion(db_impl_);

  // Drift check: fraction of rows sitting in files born or dead since the
  // last rebuild, from in-memory metadata only (no table opens).
  std::unordered_map<uint64_t, uint64_t> live;
  uint64_t live_entries = 0;
  const VersionStorageInfo* storage = sv->current->storage_info();
  for (int level = 0; level < storage->num_non_empty_levels(); ++level) {
    for (const FileMetaData* meta : storage->LevelFiles(level)) {
      live.emplace(meta->fd.GetNumber(), meta->num_entries);
      live_entries += meta->num_entries;
    }
  }
  uint64_t changed = 0;
  for (const auto& [file, entries] : live)
    if (!built_files_.count(file)) changed += entries;
  for (const auto& [file, entries] : built_files_)
    if (!live.count(file)) changed += entries;

  bool fresh_enough =
      built_entries_ > 0 &&
      static_cast<double>(changed) <
          kStaleRowFraction * static_cast<double>(built_entries_);
  if (!fresh_enough) {
    std::shared_ptr<const GlobalStats> stats = Rebuild(sv);
    built_files_ = std::move(live);
    built_entries_ = live_entries;
    lock_guard<mutex> lock(mu_);
    cached_ = std::move(stats);
  }

  if (sv->Unref()) {
    db_impl_->mutex()->Lock();
    sv->Cleanup();
    db_impl_->mutex()->Unlock();
    delete sv;
  }
}

namespace {

// Per-attr okey window accumulated from single-condition clauses; conditions
// on the same attr intersect here instead of multiplying, so BETWEEN-shaped
// CNF (a >= x, a <= y as two clauses) is estimated as one range.
struct OkeyWindow {
  uint64_t lo = 0;
  uint64_t hi = UINT64_MAX;
  bool empty = false;  // a strict bound fell off the okey domain edge

  void Apply(CompareOp op, uint64_t okey) {
    switch (op) {
      case CompareOp::EQUAL:
        lo = std::max(lo, okey);
        hi = std::min(hi, okey);
        break;
      case CompareOp::GREATER:
        if (okey == UINT64_MAX)
          empty = true;
        else
          lo = std::max(lo, okey + 1);
        break;
      case CompareOp::GREATER_EQUAL:
        lo = std::max(lo, okey);
        break;
      case CompareOp::LESS:
        if (okey == 0)
          empty = true;
        else
          hi = std::min(hi, okey - 1);
        break;
      case CompareOp::LESS_EQUAL:
        hi = std::min(hi, okey);
        break;
    }
  }
};

// Standalone selectivity of one condition (used for OR-clause members).
// Returns -1 when the condition is unestimatable (caller flags fallback).
double ConditionSelectivity(const SABICondition& cond, const GlobalStats& stats,
                            const std::vector<AttrRole>& roles, double phys) {
  if (cond.attr_idx >= roles.size()) return -1;
  if (roles[cond.attr_idx] == AttrRole::ORDERED) {
    const auto& ord = stats.ordered[cond.attr_idx];
    if (!ord.has_value()) return -1;
    OkeyWindow w;
    w.Apply(cond.op, cond.okey);
    if (w.empty || w.lo > w.hi) return 0;
    return ord->PointAwareRangeMass(w.lo, w.hi) / phys;
  }
  if (cond.op != CompareOp::EQUAL) return -1;
  const auto& uno = stats.unordered[cond.attr_idx];
  if (!uno.has_value()) return -1;
  auto it = uno->value_counts.find(cond.bytes);
  if (it != uno->value_counts.end()) return it->second / phys;
  // Absent from an exact dictionary = provably matchless in live SSTs;
  // absent after truncation only means "not top-k".
  return uno->truncated ? -1 : 0;
}

}  // namespace

EstimateResult CardinalityEstimator::Estimate(const SABIQuery& q) {
  std::shared_ptr<const GlobalStats> stats = Stats();
  EstimateResult res;
  res.physical_rows = stats->physical_rows;

  if (stats->live_sst_count == 0 || stats->physical_rows == 0) {
    // No flushed rows to estimate from; flag every queried attr.
    return FallbackResult(q);
  }
  std::set<uint32_t> fallback;

  double phys = static_cast<double>(stats->physical_rows);
  double product = 1.0;
  std::map<uint32_t, OkeyWindow> windows;

  for (const auto& clause : q.clause_groups) {
    if (clause.empty()) continue;  // trivially satisfiable
    if (clause.size() == 1) {
      const SABICondition& cond = clause[0];
      if (cond.attr_idx < schema_.attr_num() &&
          schema_.roles[cond.attr_idx] == AttrRole::ORDERED) {
        windows[cond.attr_idx].Apply(cond.op, cond.okey);
      } else {
        double f = ConditionSelectivity(cond, *stats, schema_.roles, phys);
        if (f < 0)
          fallback.insert(cond.attr_idx);
        else
          product *= f;
      }
    } else {
      // OR clause: union bound capped at 1. One unestimatable member makes
      // the whole clause unbounded, so it contributes factor 1 and a flag.
      double sum = 0;
      bool clause_fallback = false;
      for (const SABICondition& cond : clause) {
        double f = ConditionSelectivity(cond, *stats, schema_.roles, phys);
        if (f < 0) {
          clause_fallback = true;
          fallback.insert(cond.attr_idx);
        } else {
          sum += f;
        }
      }
      if (!clause_fallback) product *= std::min(1.0, sum);
    }
  }

  for (const auto& [attr_idx, w] : windows) {
    const auto& ord = stats->ordered[attr_idx];
    if (!ord.has_value()) {
      fallback.insert(attr_idx);
      continue;
    }
    if (w.empty || w.lo > w.hi)
      product = 0;
    else
      product *= ord->PointAwareRangeMass(w.lo, w.hi) / phys;
  }

  // Never estimate below one matching row: 0 is absorbing in cost math.
  res.selectivity = std::min(1.0, std::max(product, 1.0 / phys));
  res.fallback_attrs.assign(fallback.begin(), fallback.end());
  return res;
}

std::shared_ptr<const GlobalStats> CardinalityEstimator::Rebuild(
    SuperVersion* sv) {
  const uint32_t attr_num = schema_.attr_num();
  auto stats = std::make_shared<GlobalStats>();
  stats->ordered.resize(attr_num);
  stats->unordered.resize(attr_num);

  // Ordered histograms are buffered so the grid domain can be derived from
  // the live min/max okey before projecting; unordered counts merge inline.
  vector<vector<OrderedAttrHistogram>> hists(attr_num);
  vector<unordered_map<string, double>> value_maps(attr_num);
  vector<double> unordered_totals(attr_num, 0);

  TableCache* tc = sv->cfd->table_cache();
  const VersionStorageInfo* storage = sv->current->storage_info();
  const InternalKeyComparator* icmp = storage->InternalComparator();
  TableCache::CacheInterface cache_interface = tc->get_cache();

  for (int level = 0; level < storage->num_non_empty_levels(); ++level) {
    for (FileMetaData* meta : storage->LevelFiles(level)) {
      TableCache::TypedHandle* handle = nullptr;
      Status s = tc->FindTable(ReadOptions(), FileOptions(), *icmp, *meta,
                               &handle, sv->mutable_cf_options);
      if (!s.ok()) continue;  // unreadable file contributes nothing
      auto* bbt = static_cast<BlockBasedTable*>(cache_interface.Value(handle));
      auto* index_reader = bbt->get_rep()->index_reader.get();
      auto* udi = index_reader ? index_reader->GetUDIReader() : nullptr;
      if (udi == nullptr) {
        // Embedded reality (vs the standalone all-SABI guarantee): an SST
        // built while the CF was not yet bound -- a WAL-recovery flush or an
        // early compaction at DB open -- carries no SABI block. Planning
        // stats must degrade, never crash: its rows are fetch candidates all
        // the same, so count them into the physical total, but no attr stats
        // exist to harvest.
        stats->physical_rows += meta->num_entries - meta->num_deletions;
        stats->live_sst_count++;
        cache_interface.Release(handle);
        continue;
      }
      auto* reader = static_cast<SABIReader*>(udi);

      if (!reader->data_entries_cnt_psum.empty()) {
        // Tombstones are deletion markers, not fetchable rows; shadowed old
        // versions stay counted (the read path fetches them too).
        stats->physical_rows +=
            reader->data_entries_cnt_psum.back() -
            reader->bitmap_index.tombstone_bitmap.cardinality();
      }
      stats->live_sst_count++;

      for (uint32_t a = 0; a < attr_num; ++a) {
        if (schema_.roles[a] == AttrRole::ORDERED) {
          OrderedAttrHistogram h;
          if (reader->OrderedHistogram(a, &h)) hists[a].push_back(std::move(h));
        } else {
          UnorderedAttrValueCounts c;
          if (reader->UnorderedValueCounts(a, &c)) {
            for (auto& [value, count] : c.value_counts) {
              value_maps[a][value] += count;
              unordered_totals[a] += count;
            }
          }
        }
      }
      cache_interface.Release(handle);
    }
  }

  for (uint32_t a = 0; a < attr_num; ++a) {
    if (schema_.roles[a] == AttrRole::ORDERED) {
      if (hists[a].empty()) continue;
      GlobalOrderedStats ord;
      ord.min_okey = UINT64_MAX;
      ord.max_okey = 0;
      for (const auto& h : hists[a]) {
        ord.min_okey = std::min(ord.min_okey, h.boundaries.front());
        ord.max_okey = std::max(ord.max_okey, h.boundaries.back());
        // Union NDV >= any per-SST distinct count: a safe lower bound (and
        // exact when every SST sees the full value set, e.g. uniform data).
        ord.ndv = std::max(ord.ndv, h.distinct);
      }
      vector<double> cells(grid_cells_, 0);
      for (const auto& h : hists[a])
        ProjectHistogram(h, ord.min_okey, ord.max_okey, cells);
      ord.cell_psum.resize(cells.size());
      double acc = 0;
      for (size_t c = 0; c < cells.size(); ++c) {
        acc += cells[c];
        ord.cell_psum[c] = acc;
      }
      ord.total = acc;
      stats->ordered[a] = std::move(ord);
    } else {
      if (value_maps[a].empty()) continue;
      GlobalUnorderedStats uno;
      uno.total = unordered_totals[a];
      if (value_maps[a].size() > kMaxTrackedValues) {
        // NDV cap: demote to top-k by count and say so; total keeps the
        // full mass so untracked lookups can still be flagged as fallback.
        vector<pair<string, double>> entries(value_maps[a].begin(),
                                             value_maps[a].end());
        nth_element(
            entries.begin(), entries.begin() + kMaxTrackedValues, entries.end(),
            [](const auto& x, const auto& y) { return x.second > y.second; });
        entries.resize(kMaxTrackedValues);
        uno.value_counts.insert(entries.begin(), entries.end());
        uno.truncated = true;
      } else {
        uno.value_counts = std::move(value_maps[a]);
      }
      stats->unordered[a] = std::move(uno);
    }
  }
  return stats;
}

}  // namespace bit_lsm
