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
  // Only used when BitLSMOptions::ondemand_index is set: per-SSTable
  // directories that outlive block cache eviction, and the cache the blob
  // pages share with the data blocks.
  SABIDirectoryRegistry sabi_registry_;
  std::shared_ptr<rocksdb::Cache> block_cache_;

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
  // Flushes the default CF's memtable to an SST (blocks until done), so the
  // rows just written become visible through the SABI bitmap path.
  rocksdb::Status Flush();
  // With the defaults (default CF, Verified mode, no injected snapshot)
  // this behaves exactly like the original standalone entry point.
  std::unique_ptr<BitLSMIterator> NewIterator(
      BitLSMQuery& query, rocksdb::ColumnFamilyHandle* cfh = nullptr,
      ResultMode result_mode = ResultMode::Verified,
      const rocksdb::Snapshot* snapshot = nullptr);

  // To use RocksDB API
  rocksdb::DB* GetInternalDB() { return db_; }

  // Resident bytes the on-demand directory registry holds outside the block
  // cache budget, so a run can report the figure rather than assume it is
  // negligible. Zero unless BitLSMOptions::ondemand_index is set.
  size_t IndexRegistryMemoryUsage() const {
    return sabi_registry_.ApproximateMemoryUsage();
  }
  size_t IndexRegistrySize() const { return sabi_registry_.Size(); }

  // Live-set cardinality statistics for planning (see bit_lsm_estimator.h).
  // nullptr unless BitLSMOptions::enable_estimator is set.
  CardinalityEstimator* Estimator() { return estimator_.get(); }

  // Planning-time cardinality estimate against the default CF's live SST
  // set; see EstimateResult for the consumption contract. With the
  // estimator disabled every queried attr is flagged as fallback.
  EstimateResult EstimateSelectivity(const SABIQuery& q) {
    return estimator_ ? estimator_->Estimate(q)
                      : CardinalityEstimator::FallbackResult(q);
  }

  // For debug
  void Statistics();
  // ...
};
}  // namespace bit_lsm