#pragma once

#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <string>

#include "bit_lsm_estimator.h"
#include "bit_lsm_iterator.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"  // Attr

namespace bit_lsm {

class BitLSM {
 private:
  rocksdb::DB* db_;
  std::string db_path_;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  rocksdb::Options rocksdb_options_;
  BitLSMOptions bit_lsm_options_;
  ValueLayout layout_;
  std::unique_ptr<CardinalityEstimator> estimator_;
  std::shared_ptr<StatsRefreshListener> stats_listener_;

  // Helper functions

 public:
  BitLSM(const std::string& db_path, const BitLSMOptions& bit_lsm_options,
         const rocksdb::Options& rocksdb_options,
         const rocksdb::BlockBasedTableOptions& table_options);
  ~BitLSM();
  // BitLSM core API
  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload);
  rocksdb::Status PutBatch(const std::vector<std::string>& pks,
                           const std::vector<std::vector<Attr>>& attrs_list,
                           const std::vector<std::string>& payloads);
  rocksdb::Status Delete(const std::string& key);
  // With the defaults (default CF, Verified mode, no injected snapshot)
  // this behaves exactly like the original standalone entry point.
  std::unique_ptr<BitLSMIterator> NewIterator(
      BitLSMQuery& query, rocksdb::ColumnFamilyHandle* cfh = nullptr,
      ResultMode result_mode = ResultMode::Verified,
      const rocksdb::Snapshot* snapshot = nullptr);

  // To use RocksDB API
  rocksdb::DB* GetInternalDB() { return db_; }

  // Live-set cardinality statistics for planning (see bit_lsm_estimator.h).
  // nullptr unless BitLSMOptions::enable_estimator is set.
  CardinalityEstimator* Estimator() { return estimator_.get(); }

  // Planning-time cardinality estimate for a conjunctive SABI query against
  // the default CF's live SST set. See EstimateResult for the consumption
  // contract (cost slot vs row slot, fallback flags). With the estimator
  // disabled every queried attr is flagged as fallback.
  EstimateResult EstimateSelectivity(const SABIQuery& q) {
    return estimator_ ? estimator_->Estimate(q)
                      : CardinalityEstimator::FallbackResult(q);
  }

  // For debug
  void Statistics();
  // ...
};
}  // namespace bit_lsm