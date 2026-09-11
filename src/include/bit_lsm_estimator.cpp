#include "bit_lsm_estimator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>

#include "bit_lsm_encoding.h"
#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/version_set.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

using namespace std;
using namespace rocksdb;

namespace bit_lsm {

namespace {

uint64_t SatAdd(uint64_t a, uint64_t b) {
  return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

size_t CommonPrefixLength(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  size_t k = 0;
  while (k < n && a[k] == b[k]) ++k;
  return k;
}

}  // namespace

ByteAxis ByteAxis::Fit(std::string_view min, std::string_view max,
                       uint8_t lo_char, uint8_t hi_char, size_t shortest) {
  ByteAxis axis;
  const size_t k = CommonPrefixLength(min, max);
  axis.prefix = std::string(min.substr(0, k));
  axis.lo_char = lo_char;
  axis.radix = std::max<uint32_t>(2, uint32_t{hi_char} - lo_char + 1);
  // As many digits as fit in 64 bits.
  axis.digits = 0;
  const unsigned __int128 limit = static_cast<unsigned __int128>(1) << 64;
  for (unsigned __int128 p = 1; p * axis.radix <= limit; p *= axis.radix)
    ++axis.digits;
  axis.base = axis.Window(min);
  axis.span = axis.Window(max) - axis.base;
  // One value step: a step in the last real digit of the shortest boundary
  // (the digits past it are padding), never wider than the window itself.
  const size_t pad =
      k + axis.digits > shortest ? k + axis.digits - shortest : 0;
  axis.unit = 1;
  for (size_t i = 0; i < std::min(pad, axis.digits - 1); ++i)
    axis.unit *= axis.radix;
  return axis;
}

uint64_t ByteAxis::Window(std::string_view s) const {
  uint64_t acc = 0;
  for (size_t i = 0; i < digits; ++i) {
    const size_t pos = prefix.size() + i;
    uint32_t d = 0;
    if (pos < s.size()) {
      const uint8_t c = static_cast<uint8_t>(s[pos]);
      d = c <= lo_char ? 0 : std::min<uint32_t>(c - lo_char, radix - 1);
    }
    acc = acc * radix + d;
  }
  return acc;
}

uint64_t ByteAxis::End() const { return SatAdd(span, unit); }

uint64_t ByteAxis::Rel(std::string_view s) const {
  const int c = s.substr(0, prefix.size()).compare(prefix);
  if (c < 0) return 0;
  if (c > 0) return End();
  const uint64_t w = Window(s);
  if (w < base) return 0;
  const uint64_t r = w - base;
  return r > span ? End() : r;
}

GlobalRangeStats::Window GlobalRangeStats::Place(const ByteInterval& w) const {
  Window out;
  // Ends outside [min, max] are settled by bytes, so the axis never has to
  // represent "below min" (a strict bound there must not step inward) or
  // "above max".
  if (w.lo < min)
    out.lo = 0;
  else if (w.lo > max)
    out.lo = axis.End();
  else
    out.lo = SatAdd(axis.Rel(w.lo), w.lo_open ? axis.unit : 0);
  if (w.hi_unbounded || w.hi > max)
    out.hi = axis.End();
  else if (w.hi < min)
    out.hi = 0;
  else
    out.hi = SatAdd(axis.Rel(w.hi), w.hi_open ? 0 : axis.unit);
  out.point = out.hi > out.lo && out.hi - out.lo <= axis.unit;
  out.in_span = !(w.lo < min) && out.lo <= axis.span;
  return out;
}

double GlobalRangeStats::CumBelow(uint64_t rel) const {
  if (rel == 0 || total == 0) return 0;
  if (axis.span == 0 || rel > axis.span) return total;
  // Position on the [0, cells] axis; coordinates are already min-relative,
  // so narrow spans at large magnitudes keep double precision.
  double p = static_cast<double>(rel) / static_cast<double>(axis.span) *
             static_cast<double>(cell_psum.size());
  uint32_t cell = std::min(static_cast<uint32_t>(p),
                           static_cast<uint32_t>(cell_psum.size() - 1));
  double below = cell == 0 ? 0 : cell_psum[cell - 1];
  double cell_mass = cell_psum[cell] - below;
  return below + cell_mass * (p - cell);
}

double GlobalRangeStats::RangeMass(const Window& w) const {
  if (w.hi <= w.lo || total == 0) return 0;
  double upper = w.hi > axis.span ? total : CumBelow(w.hi);
  return std::max(0.0, upper - CumBelow(w.lo));
}

double GlobalRangeStats::PointAwareRangeMass(const Window& w) const {
  double mass = RangeMass(w);
  // NDV equality floor, point windows INSIDE the live span only: the grid
  // smears mass uniformly over the span, so on a sparse domain an existing
  // point reads ~total/span_used instead of ~total/ndv. Points OUTSIDE
  // [min, max] keep their zero -- there it is proof of absence, not smear.
  // (In-span holes do get floored: NDV alone cannot tell a hole from a
  // value, and overestimating a hole is the conservative direction.) Range
  // windows integrate over the holes and need no correction (verified exact
  // on SF2 BETWEENs).
  if (w.point && ndv > 0 && w.in_span)
    mass = std::max(mass, total / double(ndv));
  return std::min(mass, total);
}

double GlobalRangeStats::CandidateMass(const Window& w,
                                       double match_mass) const {
  if (w.hi <= w.lo || sst_bins.empty()) return match_mass;
  const double unit = static_cast<double>(axis.unit);
  // Pass 1: span-uniform overlap mass per covering SST -- the weight that
  // apportions the caller's GLOBAL matching mass across SSTs (the per-SST
  // stats keep no histogram, only span + bin mass; mass is conserved).
  double overlap_total = 0;
  for (const SSTBins& s : sst_bins) {
    const uint64_t s_end = SatAdd(s.hi, axis.unit);
    if (w.hi <= s.lo || w.lo >= s_end || s.total <= 0) continue;
    double span_len = static_cast<double>(s.hi - s.lo) + unit;
    double ov_len =
        static_cast<double>(std::min(w.hi, s_end) - std::max(w.lo, s.lo));
    overlap_total += s.total * (ov_len / span_len);
  }
  // Pass 2: per covering SST, round the matching share out to prune-bin
  // boundaries. A point candidates the whole bin containing it; a range
  // overshoots by half a bin per free edge (an edge clipped by the SST span
  // sits on a pinned boundary and overshoots nothing). At least one bin is
  // touched whenever the window overlaps the span; never more than the
  // SST's binned rows.
  double cand = 0;
  for (const SSTBins& s : sst_bins) {
    const uint64_t s_end = SatAdd(s.hi, axis.unit);
    if (w.hi <= s.lo || w.lo >= s_end || s.total <= 0) continue;
    uint64_t c_lo = std::max(w.lo, s.lo);
    uint64_t c_hi = std::min(w.hi, s_end);
    double span_len = static_cast<double>(s.hi - s.lo) + unit;
    double ov_len = static_cast<double>(c_hi - c_lo);
    double match_s =
        overlap_total > 0
            ? match_mass * (s.total * (ov_len / span_len)) / overlap_total
            : 0;
    double c;
    if (w.point) {
      c = std::max(s.binmass, match_s);
    } else {
      double smear =
          ((c_lo > s.lo ? 0.5 : 0.0) + (c_hi < s_end ? 0.5 : 0.0)) * s.binmass;
      c = std::max(s.binmass, match_s + smear);
    }
    cand += std::min(c, s.total);
  }
  return std::max(cand, match_mass);
}

namespace {

// Projects one per-SST histogram onto the uniform grid over the attr's axis:
// each source bin's count is spread over the cells it overlaps, proportional
// to overlap length (uniform-within-bin assumption).
void ProjectHistogram(const RangeAttrHistogram& hist, const ByteAxis& axis,
                      vector<double>& cells) {
  const uint64_t span = axis.span;
  const size_t n = cells.size();
  if (span == 0) {
    for (uint64_t c : hist.counts) cells[0] += static_cast<double>(c);
    return;
  }
  double cell_w = static_cast<double>(span) / static_cast<double>(n);
  for (size_t b = 0; b + 1 < hist.boundaries.size(); ++b) {
    double count = static_cast<double>(hist.counts[b]);
    if (count == 0) continue;
    double s = static_cast<double>(axis.Rel(hist.boundaries[b]));
    double e = static_cast<double>(axis.Rel(hist.boundaries[b + 1]));
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
  empty->range.resize(schema_.attr_num());
  empty->equality.resize(schema_.attr_num());
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

// Standalone selectivity of one condition (used for OR-clause members).
// Returns -1 when the condition is unestimatable (caller flags fallback).
double ConditionSelectivity(const SABICondition& cond, const GlobalStats& stats,
                            const std::vector<IndexType>& index_types,
                            double phys) {
  if (cond.attr_idx >= index_types.size()) return -1;
  if (index_types[cond.attr_idx] == IndexType::kRange) {
    const auto& ord = stats.range[cond.attr_idx];
    if (!ord.has_value()) return -1;
    const GlobalRangeStats::Window win = ord->Place(cond.win);
    if (win.hi <= win.lo) return 0;
    return ord->PointAwareRangeMass(win) / phys;
  }
  const auto& uno = stats.equality[cond.attr_idx];
  if (!uno.has_value()) return -1;
  auto it = uno->value_counts.find(cond.bytes);
  if (it != uno->value_counts.end()) return it->second / phys;
  // Absent from an exact dictionary = provably matchless in live SSTs;
  // absent after truncation only means "not top-k".
  return uno->truncated ? -1 : 0;
}

// Candidate-fraction counterpart of ConditionSelectivity, for a condition
// whose match fraction f is already known (>= 0): what the bin-granular
// pruning ADMITS for this condition alone.
double ConditionCandidate(const SABICondition& cond, const GlobalStats& stats,
                          const std::vector<IndexType>& index_types,
                          double phys, double f) {
  if (index_types[cond.attr_idx] == IndexType::kRange) {
    const auto& ord = stats.range[cond.attr_idx];
    const GlobalRangeStats::Window win = ord->Place(cond.win);
    if (win.hi <= win.lo) return 0;
    return std::min(1.0, ord->CandidateMass(win, f * phys) / phys);
  }
  // kEquality equality: provable absence prunes every SST at execution;
  // otherwise the value's whole balance-packed bin per SST is fetched, and
  // a value hotter than the average bin floors at its own match mass.
  if (f == 0) return 0;
  const auto& uno = stats.equality[cond.attr_idx];
  return std::min(1.0, std::max(f, uno->binmass_sum / phys));
}

}  // namespace

EstimateResult CardinalityEstimator::Estimate(const SABIQuery& q) {
  std::shared_ptr<const GlobalStats> stats = Stats();
  // Live per call, never from the rebuilt snapshot: the memtable count
  // moves with every write and drops to zero at flush, while stats lag by
  // design (bounded staleness).
  uint64_t memtable = MemtableEntries();

  if (stats->live_sst_count == 0 || stats->physical_rows == 0) {
    // No flushed rows to estimate from; flag every queried attr. The
    // memtable term still rides along -- pre-first-flush it is the ONLY
    // fetch mass there is.
    EstimateResult res = FallbackResult(q);
    res.memtable_entries = memtable;
    return res;
  }
  EstimateResult res;
  res.physical_rows = stats->physical_rows;
  res.memtable_entries = memtable;
  std::set<uint32_t> fallback;

  double phys = static_cast<double>(stats->physical_rows);
  double product = 1.0;
  double cand_product = 1.0;  // bin-rounded counterpart of `product`
  std::map<uint32_t, ByteInterval> windows;

  for (const auto& clause : q.clause_groups) {
    if (clause.empty()) continue;  // trivially satisfiable
    if (clause.size() == 1) {
      const SABICondition& cond = clause[0];
      if (cond.attr_idx < schema_.attr_num() &&
          schema_.index_types[cond.attr_idx] == IndexType::kRange) {
        windows[cond.attr_idx].Intersect(cond.win);
      } else {
        double f =
            ConditionSelectivity(cond, *stats, schema_.index_types, phys);
        if (f < 0) {
          fallback.insert(cond.attr_idx);
        } else {
          product *= f;
          cand_product *=
              ConditionCandidate(cond, *stats, schema_.index_types, phys, f);
        }
      }
    } else {
      // OR clause: union bound capped at 1, for both slots. One
      // unestimatable member makes the whole clause unbounded, so it
      // contributes factor 1 and a flag.
      double sum = 0;
      double cand_sum = 0;
      bool clause_fallback = false;
      for (const SABICondition& cond : clause) {
        double f =
            ConditionSelectivity(cond, *stats, schema_.index_types, phys);
        if (f < 0) {
          clause_fallback = true;
          fallback.insert(cond.attr_idx);
        } else {
          sum += f;
          cand_sum +=
              ConditionCandidate(cond, *stats, schema_.index_types, phys, f);
        }
      }
      if (!clause_fallback) {
        product *= std::min(1.0, sum);
        cand_product *= std::min(1.0, cand_sum);
      }
    }
  }

  for (const auto& [attr_idx, w] : windows) {
    const auto& ord = stats->range[attr_idx];
    if (!ord.has_value()) {
      fallback.insert(attr_idx);
      continue;
    }
    const GlobalRangeStats::Window win = ord->Place(w);
    if (win.hi <= win.lo) {
      product = 0;
      cand_product = 0;
    } else {
      double match_mass = ord->PointAwareRangeMass(win);
      product *= match_mass / phys;
      cand_product *= std::min(1.0, ord->CandidateMass(win, match_mass) / phys);
    }
  }

  // Never estimate below one matching row: 0 is absorbing in cost math.
  res.selectivity = std::min(1.0, std::max(product, 1.0 / phys));
  // Candidates are a superset of matches; the (floored) selectivity also
  // carries the >= 1-row floor into the cost slot.
  res.candidate_selectivity =
      std::min(1.0, std::max(cand_product, res.selectivity));
  res.fallback_attrs.assign(fallback.begin(), fallback.end());
  return res;
}

uint64_t CardinalityEstimator::MemtableEntries() {
  SuperVersion* sv = db_impl_->GetAndRefSuperVersion(cfd_);
  if (sv == nullptr) return 0;
  uint64_t n = sv->mem->NumEntries() + sv->imm->GetTotalNumEntries();
  db_impl_->ReturnAndCleanupSuperVersion(cfd_, sv);
  return n;
}

std::shared_ptr<const GlobalStats> CardinalityEstimator::Rebuild(
    SuperVersion* sv) {
  const uint32_t attr_num = schema_.attr_num();
  auto stats = std::make_shared<GlobalStats>();
  stats->range.resize(attr_num);
  stats->equality.resize(attr_num);

  // Range histograms are buffered so the axis can be derived from the live
  // global bounds before projecting; equality counts merge inline.
  vector<vector<RangeAttrHistogram>> hists(attr_num);
  vector<unordered_map<string, double>> value_maps(attr_num);
  vector<double> unordered_totals(attr_num, 0);
  vector<double> unordered_binmass_sums(attr_num, 0);

  TableCache* tc = sv->cfd->table_cache();
  const VersionStorageInfo* storage = sv->current->storage_info();
  const InternalKeyComparator* icmp = storage->InternalComparator();
  TableCache::CacheInterface cache_interface = tc->get_cache();

  for (int level = 0; level < storage->num_non_empty_levels(); ++level) {
    for (FileMetaData* meta : storage->LevelFiles(level)) {
      TableCache::TypedHandle* handle = nullptr;
      Status s = tc->FindTable(ReadOptions(), *sv->cfd->soptions(), *icmp,
                               *meta, &handle, sv->mutable_cf_options);
      if (!s.ok()) continue;  // unreadable file contributes nothing
      auto* bbt = static_cast<BlockBasedTable*>(cache_interface.Value(handle));
      // Holds the SABI entry for this file's harvest: a block cache pin when
      // cache_index_and_filter_blocks is on (evictable after release), or an
      // unowned reference to the table-lifetime pin in Rep when off. Either
      // way valid only while the table reader stays alive. Everything
      // harvested below is copied out, so it only has to outlive this
      // iteration.
      CachableEntry<Block_kUserDefinedIndex> udi_entry;
      Status udi_s = bbt->GetUserDefinedIndexReader(ReadOptions(), &udi_entry);
      if (!udi_s.ok()) {
        // Embedded reality (vs the standalone all-SABI guarantee): an SST
        // built while the CF was not yet bound -- a WAL-recovery flush or an
        // early compaction at DB open -- carries no SABI block (NotFound), and
        // a damaged one fails to parse (Corruption). Any other failure to
        // obtain the reader, an IO error on the re-read included, degrades the
        // same way: planning stats must degrade, never crash. The file's rows
        // are fetch candidates all the same, so count them into the physical
        // total, but no attr stats are available to harvest.
        stats->physical_rows += meta->num_entries - meta->num_deletions;
        stats->live_sst_count++;
        cache_interface.Release(handle);
        continue;
      }
      auto* reader = static_cast<SABIReader*>(udi_entry.GetValue()->reader());

      if (!reader->data_entries_cnt_psum.empty()) {
        // Tombstones are deletion markers, not fetchable rows; shadowed old
        // versions stay counted (the read path fetches them too).
        stats->physical_rows += reader->data_entries_cnt_psum.back() -
                                reader->TombstoneCardinality();
      }
      stats->live_sst_count++;

      for (uint32_t a = 0; a < attr_num; ++a) {
        if (schema_.index_types[a] == IndexType::kRange) {
          RangeAttrHistogram h;
          if (reader->RangeHistogram(a, &h)) hists[a].push_back(std::move(h));
        } else {
          EqualityAttrValueCounts c;
          if (reader->EqualityValueCounts(a, &c)) {
            double sst_total = 0;
            for (auto& [value, count] : c.value_counts) {
              value_maps[a][value] += count;
              sst_total += count;
            }
            unordered_totals[a] += sst_total;
            // Candidate scalar: one balance-packed bin of this SST is what
            // an equality on any tracked value fetches here.
            if (a < reader->bitmap_index.bitmap_nums.size() &&
                reader->bitmap_index.bitmap_nums[a] > 0) {
              unordered_binmass_sums[a] +=
                  sst_total / reader->bitmap_index.bitmap_nums[a];
            }
          }
        }
      }
      cache_interface.Release(handle);
    }
  }

  for (uint32_t a = 0; a < attr_num; ++a) {
    if (schema_.index_types[a] == IndexType::kRange) {
      if (hists[a].empty()) continue;
      GlobalRangeStats ord;
      // Axis first: exact global bounds, the byte range the boundaries use
      // past their common prefix, and the shortest boundary (one value step).
      bool first = true;
      for (const auto& h : hists[a]) {
        if (first || h.boundaries[0] < ord.min)
          ord.min = std::string(h.boundaries[0]);
        if (first || h.boundaries.back() > ord.max)
          ord.max = std::string(h.boundaries.back());
        first = false;
        // Union NDV >= any per-SST distinct count: a safe lower bound (and
        // exact when every SST sees the full value set, e.g. uniform data).
        ord.ndv = std::max(ord.ndv, h.distinct);
      }
      const size_t k = CommonPrefixLength(ord.min, ord.max);
      uint8_t lo_char = 255, hi_char = 0;
      size_t shortest = SIZE_MAX;
      for (const auto& h : hists[a]) {
        for (size_t b = 0; b < h.boundaries.size(); ++b) {
          const std::string_view v = h.boundaries[b];
          shortest = std::min(shortest, v.size());
          for (size_t i = k; i < v.size(); ++i) {
            const uint8_t c = static_cast<uint8_t>(v[i]);
            lo_char = std::min(lo_char, c);
            hi_char = std::max(hi_char, c);
          }
        }
      }
      if (lo_char > hi_char) lo_char = hi_char = 0;  // no byte past the prefix
      ord.axis = ByteAxis::Fit(ord.min, ord.max, lo_char, hi_char, shortest);

      ord.sst_bins.reserve(hists[a].size());
      for (const auto& h : hists[a]) {
        GlobalRangeStats::SSTBins b;
        b.lo = ord.axis.Rel(h.boundaries[0]);
        b.hi = ord.axis.Rel(h.boundaries.back());
        for (uint64_t c : h.counts) b.total += static_cast<double>(c);
        if (!h.counts.empty()) b.binmass = b.total / h.counts.size();
        ord.sst_bins.push_back(b);
      }
      vector<double> cells(grid_cells_, 0);
      for (const auto& h : hists[a]) ProjectHistogram(h, ord.axis, cells);
      ord.cell_psum.resize(cells.size());
      double acc = 0;
      for (size_t c = 0; c < cells.size(); ++c) {
        acc += cells[c];
        ord.cell_psum[c] = acc;
      }
      ord.total = acc;
      stats->range[a] = std::move(ord);
    } else {
      if (value_maps[a].empty()) continue;
      GlobalEqualityStats uno;
      uno.total = unordered_totals[a];
      uno.binmass_sum = unordered_binmass_sums[a];
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
      stats->equality[a] = std::move(uno);
    }
  }
  return stats;
}

}  // namespace bit_lsm
