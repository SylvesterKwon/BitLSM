#include "bit_lsm_estimator.h"

#include <algorithm>
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
  // Position on the [0, cells] axis; s <= span < 2^64 so the double
  // conversion works on the shifted span, not the okey magnitude (C5).
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

namespace {

// Projects one per-SST histogram onto the uniform grid over [min_okey,
// max_okey]: each source bin's count is spread over the cells it overlaps,
// proportional to overlap length (uniform-within-bin assumption). All
// arithmetic runs on min_okey-shifted coordinates (C5).
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
                                           SABISchema schema)
    : db_impl_(db_impl), cfd_(cfd), schema_(std::move(schema)) {}

std::shared_ptr<const GlobalStats> CardinalityEstimator::Get() {
  lock_guard<mutex> lock(mu_);
  if (cached_ && built_sv_number_ == cfd_->GetSuperVersionNumber())
    return cached_;

  SuperVersion* sv = cfd_->GetReferencedSuperVersion(db_impl_);
  cached_ = Rebuild(sv);
  // Record the number of the SuperVersion actually read: if a newer one
  // landed between the staleness check and the ref, the next Get() rebuilds.
  built_sv_number_ = sv->version_number;
  if (sv->Unref()) {
    db_impl_->mutex()->Lock();
    sv->Cleanup();
    db_impl_->mutex()->Unlock();
    delete sv;
  }
  return cached_;
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
    return ord->RangeMass(w.lo, w.hi) / phys;
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
  std::shared_ptr<const GlobalStats> stats = Get();
  EstimateResult res;
  res.physical_rows = stats->physical_rows;

  std::set<uint32_t> fallback;
  if (stats->live_sst_count == 0 || stats->physical_rows == 0) {
    // D-E3 documented limit: before the first flush (or on an empty CF)
    // there is nothing to estimate from; flag every queried attr.
    for (const auto& clause : q.clause_groups)
      for (const auto& cond : clause) fallback.insert(cond.attr_idx);
    res.fallback_attrs.assign(fallback.begin(), fallback.end());
    return res;
  }

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
      product *= ord->RangeMass(w.lo, w.hi) / phys;
  }

  res.selectivity = std::min(1.0, std::max(0.0, product));
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
      auto* reader = static_cast<SABIReader*>(
          bbt->get_rep()->index_reader->GetUDIReader());

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
      }
      vector<double> cells(kGridCells, 0);
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
