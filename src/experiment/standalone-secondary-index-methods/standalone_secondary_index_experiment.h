#pragma once

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/utilities/transaction_db.h"
#include <cstdint>
#include <iostream>
#include <vector>

// Interface for standalone, secondary index experiment
class StandaloneSecondaryIndexExperiment {
private:
  std::string db_path_ = "/scratch/data/eager_updates";

protected:
  rocksdb::TransactionDB* txn_db;
  rocksdb::Options options;
  rocksdb::TransactionDBOptions txn_db_options;
  std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
  rocksdb::Status s;
  StandaloneSecondaryIndexExperiment() = default;
  uint32_t si_cnt;

  const std::string primary_index_cf_name = rocksdb::kDefaultColumnFamilyName;
  const std::string secondary_index_cf_name = "secondary_index";

  virtual void ConfigureCustomDBOptions() {};

  void Initialize(std::string db_path, uint32_t si_cnt) {
    std::cout << "Initialize StandaloneSecondaryIndexExperiment\n";
    db_path_ = db_path;
    this->si_cnt = si_cnt;

    // destroy previous db
    // std::cout << "Destroying previous DB...\n";
    // s = rocksdb::DestroyDB(db_path_, options);

    // configure common DB setting
    options.create_if_missing = true;
    options.create_missing_column_families = true;

    // Tuning Rocksdb for heavy workload
    options.max_write_buffer_number = 6;          // default 2
    options.min_write_buffer_number_to_merge = 2; // default 1
    options.max_background_jobs = 8;              // default 2

    rocksdb::ColumnFamilyOptions cf_options(options);
    column_families = {
        rocksdb::ColumnFamilyDescriptor(primary_index_cf_name, cf_options),
        rocksdb::ColumnFamilyDescriptor(secondary_index_cf_name, cf_options),
    };

    // configure children class DB setting
    std::cout << "Configuring custom DB options...\n";
    ConfigureCustomDBOptions();

    std::cout << "Opening DB...\n";
    s = rocksdb::TransactionDB::Open(options, txn_db_options, db_path_,
                                     column_families, &cf_handles, &txn_db);
    if (!s.ok())
      std::cout << "state: " << s.getState() << "\n";
    assert(s.ok());

    std::cout << "Succesfully opened DB.\n";
  }

public:
  virtual ~StandaloneSecondaryIndexExperiment() {
    std::cout << "Shutting down DB gracefully...\n";

    for (auto cf_handle : cf_handles) {
      s = txn_db->DestroyColumnFamilyHandle(cf_handle);
      assert(s.ok());
    }

    rocksdb::WaitForCompactOptions wait_for_compact_options =
        rocksdb::WaitForCompactOptions();
    wait_for_compact_options.close_db = true;
    s = txn_db->WaitForCompact(wait_for_compact_options);
    assert(s.ok());
    delete txn_db;

    std::cout << "Successfully closed DB. Bye!\n";
  };

  enum IndexType {
    kPrimaryIndex,
    kSecondaryIndex,
  };
  template <typename T>
  static std::unique_ptr<T> Create(const std::string& db_path_,
                                   uint32_t si_cnt = 1) {
    auto ptr = std::make_unique<T>();
    ptr->Initialize(db_path_, si_cnt);
    return ptr;
  }
  rocksdb::Status Get(const rocksdb::Slice& key, std::string* value) {
    rocksdb::ReadOptions read_options;
    return s = txn_db->Get(read_options, cf_handles[0], key, value);
  };
  virtual rocksdb::Status Insert(const rocksdb::Slice& key,
                                 const rocksdb::Slice& value) = 0;
  virtual rocksdb::Status GetBySecondaryIndex(
      const uint32_t idx_no, const rocksdb::Slice& key,
      std::vector<std::pair<std::string, std::string>>* results) = 0;
  enum CompositeQueryRunStrategy {
    kIndexMerge,
    kPostFiltering, // will respect given query order
    kFullTableScan,
  };
  virtual std::vector<CompositeQueryRunStrategy>
  GetAvailableCompositeQueryRunStrategy() const {
    return {};
  };
  // Perform composite query based on given query conditions. All conditions are
  // logically ANDed.
  virtual rocksdb::Status GetBySecondaryIndices(
      const std::vector<std::pair<uint32_t, rocksdb::Slice>>& query,
      CompositeQueryRunStrategy strategy,
      std::vector<std::pair<std::string, std::string>>* results) = 0;
  // virtual rocksdb::Status Delete(const rocksdb::Slice& key);
};
