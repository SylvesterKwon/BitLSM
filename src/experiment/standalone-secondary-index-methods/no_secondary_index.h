#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "standalone_secondary_index_experiment.h"
#include "standalone_secondary_index_utils.h"
#include <cstring>

using namespace std;
using namespace rocksdb;

class NoSecondaryIndex : public StandaloneSecondaryIndexExperiment {
public:
  Status Insert(const Slice& key, const Slice& value) override {
    WriteOptions write_options;
    Transaction* txn = txn_db->BeginTransaction(write_options);

    // 1. Update PK index
    s = txn->Put(cf_handles[0], key, value);

    // 2. Commit transaction
    s = txn->Commit();
    assert(s.ok());
    delete txn;

    return Status::OK();
  };

  Status GetBySecondaryIndex(const uint32_t idx_no, const Slice& key,
                             vector<pair<string, string>>* results) override {
    // 1. Get PK list by SK (full table scan)
    ReadOptions read_options;
    read_options.pin_data = true;
    Iterator* it = txn_db->NewIterator(read_options, cf_handles[0]);
    results->clear();

    vector<string> pk_str_list;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      string target_sk = GetIthToken(it->value().ToString(), idx_no, ',');
      if (key.compare(target_sk))
        continue;
      results->push_back({it->key().ToString(), it->value().ToString()});
    }
    delete it;

    return Status::OK();
  };
};