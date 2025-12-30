#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/utilities/transaction_db.h"
#include <iostream>

// Interface for standalone, secondary index experiment
class StandaloneSecondaryIndexExperiment {
private:
  std::string db_path_ = "/scratch/data/eager_updates";

protected:
  rocksdb::TransactionDB* txn_db;
  rocksdb::Options options;
  rocksdb::TransactionDBOptions txn_db_options;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
  rocksdb::Status s;
  StandaloneSecondaryIndexExperiment() = default;

  const std::string primary_index_cf_name = rocksdb::kDefaultColumnFamilyName;
  const std::string secondary_index_cf_name = "secondary_index";

  void Initialize(std::string db_path) {
    std::cout << "Initialize StandaloneSecondaryIndexExperiment\n";
    db_path_ = db_path;

    // destroy previous db
    std::cout << "Destroying previous DB...\n";
    s = rocksdb::DestroyDB(db_path_, options);

    // configure DB
    options.create_if_missing = true;
    options.create_missing_column_families = true;
    std::vector<rocksdb::ColumnFamilyDescriptor> column_families = {
        rocksdb::ColumnFamilyDescriptor(primary_index_cf_name,
                                        rocksdb::ColumnFamilyOptions()),
        rocksdb::ColumnFamilyDescriptor(secondary_index_cf_name,
                                        rocksdb::ColumnFamilyOptions()),
    };

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
  static std::unique_ptr<T> Create(const std::string& db_path_) {
    auto ptr = std::make_unique<T>();
    ptr->Initialize(db_path_);
    return ptr;
  }
  virtual rocksdb::Status Insert(const rocksdb::Slice& key,
                                 const rocksdb::Slice& value) = 0;
  virtual rocksdb::Status Get(const rocksdb::Slice& key,
                              std::string* value) = 0;
  virtual std::vector<rocksdb::Status> GetBySecondaryIndex(
      const rocksdb::Slice& key,
      std::vector<std::pair<std::string, rocksdb::PinnableSlice>>* results) = 0;
  // virtual rocksdb::Status Delete(const rocksdb::Slice& key);
};
