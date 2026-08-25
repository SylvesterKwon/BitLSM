#pragma once

#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <map>
#include <mutex>
#include <string>

#include "bit_lsm_column_family.h"
#include "bit_lsm_estimator.h"
#include "bit_lsm_iterator.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"  // Attr

namespace bit_lsm {

class BitLSM {
 private:
  rocksdb::DB* db_;
  std::string db_path_;
  rocksdb::Options rocksdb_options_;
  rocksdb::BlockBasedTableOptions table_options_;  // shared per-CF base
  std::map<std::string, std::unique_ptr<ColumnFamilyHandle>> cf_registry_;
  ColumnFamilyHandle* default_cf_ = nullptr;
  std::shared_ptr<StatsRefreshListener> stats_listener_;
  mutable std::mutex cf_mu_;  // guards cf_registry_ for Create/Drop/Get

  ColumnFamilyHandle* RegisterColumnFamily(const std::string& name,
                                           rocksdb::ColumnFamilyHandle* handle,
                                           const BitLSMOptions& options);

 public:
  // Convenience: opens the default CF only, with `bit_lsm_options` as its
  // schema. Equivalent to the descriptor form with a single default entry.
  BitLSM(const std::string& db_path, const BitLSMOptions& bit_lsm_options,
         const rocksdb::Options& rocksdb_options,
         const rocksdb::BlockBasedTableOptions& table_options);
  // Multi-CF open. RocksDB rule: descriptors must list every CF that exists
  // in the DB and must include rocksdb::kDefaultColumnFamilyName. The
  // rocksdb/table options are the shared base; each CF's SABI factory is
  // derived from its descriptor's BitLSMOptions.
  BitLSM(const std::string& db_path, const rocksdb::Options& rocksdb_options,
         const rocksdb::BlockBasedTableOptions& table_options,
         const std::vector<ColumnFamilyDescriptor>& descriptors);
  ~BitLSM();

  // Column family management (names bind here; per-op addressing is by
  // handle, mirroring RocksDB).
  ColumnFamilyHandle* DefaultColumnFamily() const { return default_cf_; }
  ColumnFamilyHandle* GetColumnFamily(const std::string& name) const;
  rocksdb::Status CreateColumnFamily(const std::string& name,
                                     const BitLSMOptions& options,
                                     ColumnFamilyHandle** out);
  rocksdb::Status DropColumnFamily(ColumnFamilyHandle* cf);
  static rocksdb::Status ListColumnFamilies(const rocksdb::Options& options,
                                            const std::string& db_path,
                                            std::vector<std::string>* out);

  // BitLSM core API. The CF-less overloads target the default CF.
  rocksdb::Status Put(ColumnFamilyHandle* cf, const std::string& pk,
                      const std::vector<Attr>& attrs,
                      const std::string& payload);
  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload) {
    return Put(default_cf_, pk, attrs, payload);
  }
  rocksdb::Status PutBatch(ColumnFamilyHandle* cf,
                           const std::vector<std::string>& pks,
                           const std::vector<std::vector<Attr>>& attrs_list,
                           const std::vector<std::string>& payloads);
  rocksdb::Status PutBatch(const std::vector<std::string>& pks,
                           const std::vector<std::vector<Attr>>& attrs_list,
                           const std::vector<std::string>& payloads) {
    return PutBatch(default_cf_, pks, attrs_list, payloads);
  }
  rocksdb::Status Delete(ColumnFamilyHandle* cf, const std::string& key);
  rocksdb::Status Delete(const std::string& key) {
    return Delete(default_cf_, key);
  }
  // Flushes the CF's memtable to an SST (blocks until done), so the rows
  // just written become visible through the SABI bitmap path.
  rocksdb::Status Flush(ColumnFamilyHandle* cf);
  rocksdb::Status Flush() { return Flush(default_cf_); }

  // Scans decode with the target CF's own schema. Candidate mode requires an
  // injected snapshot (see ResultMode).
  std::unique_ptr<BitLSMIterator> NewIterator(
      ColumnFamilyHandle* cf, BitLSMQuery& query,
      ResultMode result_mode = ResultMode::Verified,
      const rocksdb::Snapshot* snapshot = nullptr);
  std::unique_ptr<BitLSMIterator> NewIterator(
      BitLSMQuery& query, ResultMode result_mode = ResultMode::Verified,
      const rocksdb::Snapshot* snapshot = nullptr) {
    return NewIterator(default_cf_, query, result_mode, snapshot);
  }

  // To use RocksDB API
  rocksdb::DB* GetInternalDB() { return db_; }

  // Live-set cardinality statistics for planning (see bit_lsm_estimator.h).
  // nullptr unless that CF's BitLSMOptions::enable_estimator is set.
  CardinalityEstimator* Estimator() { return default_cf_->Estimator(); }

  // Planning-time cardinality estimate against the CF's live SST set; see
  // EstimateResult for the consumption contract. With the estimator disabled
  // -- or with an unknown column family -- every queried attr is flagged as
  // fallback.
  EstimateResult EstimateSelectivity(ColumnFamilyHandle* cf,
                                     const SABIQuery& q) {
    return cf && cf->Estimator() ? cf->Estimator()->Estimate(q)
                                 : CardinalityEstimator::FallbackResult(q);
  }
  EstimateResult EstimateSelectivity(const SABIQuery& q) {
    return EstimateSelectivity(default_cf_, q);
  }

  // For debug
  void Statistics();
};
}  // namespace bit_lsm
