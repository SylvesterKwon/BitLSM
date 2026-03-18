#pragma once

#include "benchmark_experiment.h"
#include "../standalone-secondary-index-methods/standalone_secondary_index_utils.h"
#include <rocksdb/merge_operator.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/transaction_db.h>
#include <algorithm>
#include <unordered_map>

namespace benchmark {

// ---------- SI ↔ Attr index mapping ----------

struct SIMapping {
  std::vector<uint32_t> si_to_attr;  // si_no → attr_idx
  std::unordered_map<uint32_t, uint32_t> attr_to_si;  // attr_idx → si_no
  uint32_t si_cnt;
};

inline SIMapping BuildSIMapping(const Schema& schema) {
  SIMapping m;
  for (uint32_t j = 0; j < schema.options.attr_num; ++j) {
    if (schema.options.attr_types[j] == bit_lsm::AttrType::CATEGORICAL) {
      m.attr_to_si[j] = static_cast<uint32_t>(m.si_to_attr.size());
      m.si_to_attr.push_back(j);
    }
  }
  m.si_cnt = static_cast<uint32_t>(m.si_to_attr.size());
  return m;
}

// ---------- Composite query strategy ----------

enum class SIStrategy { kIndexMerge, kPostFiltering };

inline SIStrategy ParseSIStrategy(const std::string& s) {
  if (s == "post_filter") return SIStrategy::kPostFiltering;
  return SIStrategy::kIndexMerge;
}

inline std::string SIStrategyToString(SIStrategy s) {
  return s == SIStrategy::kPostFiltering ? "post_filter" : "index_merge";
}

// ---------- Query → SI lookup plan ----------

struct SIQueryPlan {
  std::vector<std::pair<uint32_t, std::string>> si_lookups;  // (si_no, sk_value)
  bit_lsm::BitLSMQuery post_filter_query;  // remaining conditions
};

inline SIQueryPlan MapQueryToSILookups(const bit_lsm::BitLSMQuery& query,
                                       const SIMapping& mapping,
                                       const bit_lsm::BitLSMOptions& options) {
  SIQueryPlan plan;
  for (const auto& cond : query.conditions) {
    auto it = mapping.attr_to_si.find(cond.attr_idx);
    if (it != mapping.attr_to_si.end() &&
        cond.op == bit_lsm::CompareOp::EQUAL &&
        options.attr_types[cond.attr_idx] == bit_lsm::AttrType::CATEGORICAL) {
      plan.si_lookups.emplace_back(it->second,
                                   std::get<std::string>(cond.value));
    } else {
      plan.post_filter_query.conditions.push_back(cond);
    }
  }
  return plan;
}

// ---------- Merge operator for LazyUpdates ----------

class SIValueMergeOperator : public rocksdb::MergeOperator {
 public:
  const char* Name() const override { return "SIValueMergeOperator"; }

  bool FullMergeV2(const MergeOperationInput& merge_in,
                   MergeOperationOutput* merge_out) const override {
    std::vector<rocksdb::Slice> all_operand_list = merge_in.operand_list;
    if (merge_in.existing_value)
      all_operand_list.push_back(*merge_in.existing_value);
    MergeIndexValue(all_operand_list, &merge_out->new_value);
    return true;
  }

  bool PartialMergeMulti(const rocksdb::Slice&,
                          const std::deque<rocksdb::Slice>& operand_list,
                          std::string* new_value,
                          rocksdb::Logger*) const override {
    std::vector<rocksdb::Slice> all(operand_list.begin(), operand_list.end());
    MergeIndexValue(all, new_value);
    return true;
  }
};

// ---------- Common TransactionDB open ----------

struct SIDBHandles {
  rocksdb::TransactionDB* txn_db = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
};

inline SIDBHandles OpenSITransactionDB(
    const std::string& db_path,
    rocksdb::ColumnFamilyOptions secondary_cf_opts_override = {}) {
  SIDBHandles h;

  rocksdb::Options opts;
  opts.create_if_missing = true;
  opts.create_missing_column_families = true;
  opts.max_background_jobs = 6;
  opts.bytes_per_sync = 1048576;
  opts.compaction_pri = rocksdb::kMinOverlappingRatio;
  opts.max_write_buffer_number = 5;

  rocksdb::BlockBasedTableOptions table_options;
  table_options.block_size = 4 * 1024;
  opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));

  rocksdb::ColumnFamilyOptions primary_cf_opts(opts);
  primary_cf_opts.level_compaction_dynamic_level_bytes = true;

  // Merge secondary CF options with base
  rocksdb::ColumnFamilyOptions secondary_cf_opts(opts);
  secondary_cf_opts.level_compaction_dynamic_level_bytes = true;
  if (secondary_cf_opts_override.merge_operator)
    secondary_cf_opts.merge_operator = secondary_cf_opts_override.merge_operator;
  if (secondary_cf_opts_override.table_factory)
    secondary_cf_opts.table_factory = secondary_cf_opts_override.table_factory;
  if (secondary_cf_opts_override.prefix_extractor)
    secondary_cf_opts.prefix_extractor = secondary_cf_opts_override.prefix_extractor;

  std::vector<rocksdb::ColumnFamilyDescriptor> column_families = {
      {rocksdb::kDefaultColumnFamilyName, primary_cf_opts},
      {"secondary_index", secondary_cf_opts},
  };

  rocksdb::TransactionDBOptions txn_db_opts;
  auto s = rocksdb::TransactionDB::Open(opts, txn_db_opts, db_path,
                                         column_families, &h.cf_handles,
                                         &h.txn_db);
  if (!s.ok()) {
    std::cerr << "Failed to open TransactionDB: " << s.ToString() << "\n";
    exit(1);
  }
  return h;
}

inline void CloseSITransactionDB(SIDBHandles& h) {
  if (!h.txn_db) return;
  for (auto* handle : h.cf_handles)
    h.txn_db->DestroyColumnFamilyHandle(handle);
  h.cf_handles.clear();
  rocksdb::WaitForCompactOptions wait_opts;
  wait_opts.close_db = true;
  h.txn_db->WaitForCompact(wait_opts);
  delete h.txn_db;
  h.txn_db = nullptr;
  std::cout << "DB successfully closed\n";
}

// ---------- Common Scan helpers ----------

// IndexMerge: lookup each SI, intersect PK lists, MultiGet, post-filter
inline ReadResult ScanByIndexMerge(
    rocksdb::TransactionDB* txn_db,
    const std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles,
    SIQueryPlan& plan,
    const bit_lsm::BitLSMOptions& options,
    uint64_t n,
    // GetPKList: given (si_no, sk_value), returns vector<string> of PKs
    std::function<std::vector<std::string>(uint32_t si_no, const std::string& sk)> GetPKList) {
  auto start = std::chrono::high_resolution_clock::now();

  bool is_first = true;
  std::vector<std::string> merged;
  for (auto& [si_no, sk_value] : plan.si_lookups) {
    auto pk_list = GetPKList(si_no, sk_value);
    if (is_first) {
      merged = std::move(pk_list);
      is_first = false;
    } else {
      std::vector<std::string> tmp;
      std::set_intersection(merged.begin(), merged.end(),
                            pk_list.begin(), pk_list.end(),
                            std::back_inserter(tmp));
      merged = std::move(tmp);
    }
    if (merged.empty()) break;
  }

  // MultiGet from primary CF
  uint64_t matched = 0;
  if (!merged.empty()) {
    std::vector<rocksdb::Slice> keys(merged.size());
    for (size_t i = 0; i < merged.size(); ++i)
      keys[i] = rocksdb::Slice(merged[i]);

    std::vector<rocksdb::PinnableSlice> values(merged.size());
    std::vector<rocksdb::Status> statuses(merged.size());
    rocksdb::ReadOptions ro;
    txn_db->MultiGet(ro, cf_handles[0], merged.size(), keys.data(),
                     values.data(), statuses.data());

    for (size_t i = 0; i < merged.size(); ++i) {
      if (statuses[i].ok()) {
        if (plan.post_filter_query.conditions.empty() ||
            plan.post_filter_query.CheckCondition(
                rocksdb::Slice(values[i].data(), values[i].size()), options))
          matched++;
      }
    }
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  std::cout << "scan (index_merge) done: " << matched << "/" << n
            << " matched, " << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          n > 0 ? static_cast<double>(matched) / n : 0.0};
}

// PostFiltering: lookup first SI, MultiGet, filter all remaining conditions
inline ReadResult ScanByPostFiltering(
    rocksdb::TransactionDB* txn_db,
    const std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles,
    SIQueryPlan& plan,
    const bit_lsm::BitLSMQuery& original_query,
    const bit_lsm::BitLSMOptions& options,
    uint64_t n,
    std::function<std::vector<std::string>(uint32_t si_no, const std::string& sk)> GetPKList) {
  auto start = std::chrono::high_resolution_clock::now();

  // Use first SI lookup only
  auto pk_list = GetPKList(plan.si_lookups[0].first,
                           plan.si_lookups[0].second);

  // Build filter query: all conditions except the first SI lookup
  bit_lsm::BitLSMQuery filter_query;
  for (const auto& cond : original_query.conditions) {
    if (cond.op == bit_lsm::CompareOp::EQUAL &&
        options.attr_types[cond.attr_idx] == bit_lsm::AttrType::CATEGORICAL &&
        std::get<std::string>(cond.value) == plan.si_lookups[0].second) {
      continue;  // skip the first SI condition
    }
    filter_query.conditions.push_back(cond);
  }

  uint64_t matched = 0;
  if (!pk_list.empty()) {
    std::vector<rocksdb::Slice> keys(pk_list.size());
    for (size_t i = 0; i < pk_list.size(); ++i)
      keys[i] = rocksdb::Slice(pk_list[i]);

    std::vector<rocksdb::PinnableSlice> values(pk_list.size());
    std::vector<rocksdb::Status> statuses(pk_list.size());
    rocksdb::ReadOptions ro;
    txn_db->MultiGet(ro, cf_handles[0], pk_list.size(), keys.data(),
                     values.data(), statuses.data());

    for (size_t i = 0; i < pk_list.size(); ++i) {
      if (statuses[i].ok()) {
        if (filter_query.conditions.empty() ||
            filter_query.CheckCondition(
                rocksdb::Slice(values[i].data(), values[i].size()), options))
          matched++;
      }
    }
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  std::cout << "scan (post_filter) done: " << matched << "/" << n
            << " matched, " << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          n > 0 ? static_cast<double>(matched) / n : 0.0};
}

// Full table scan fallback
inline ReadResult ScanFullTable(
    rocksdb::TransactionDB* txn_db,
    const std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles,
    bit_lsm::BitLSMQuery& query,
    const bit_lsm::BitLSMOptions& options,
    uint64_t n) {
  rocksdb::ReadOptions ro;
  auto* it = txn_db->NewIterator(ro, cf_handles[0]);
  uint64_t matched = 0, total = 0;
  auto start = std::chrono::high_resolution_clock::now();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    total++;
    if (query.CheckCondition(it->value(), options))
      matched++;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  delete it;
  std::cout << "scan (full_table) done: " << matched << "/" << total
            << " matched, " << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          total > 0 ? static_cast<double>(matched) / total : 0.0};
}

}  // namespace benchmark
