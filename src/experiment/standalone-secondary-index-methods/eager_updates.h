#include "rocksdb/options.h"
#include "standalone_secondary_index_experiment.h"
#include <cstring>
#include <sstream>

using namespace std;
using namespace rocksdb;

class EagerUpdates : public StandaloneSecondaryIndexExperiment {
public:
  Status Insert(const Slice& key, const Slice& value) {
    ReadOptions read_options;
    WriteOptions write_options;
    Transaction* txn = txn_db->BeginTransaction(write_options);
    string_view current_sk =
        value.ToStringView().substr(0, value.ToStringView().find(','));

    // 1. Find existing SK entry
    string existing_sk_value; // {sk_i, {pks...}}
    s = txn_db->Get(read_options, cf_handles[0], key, &existing_sk_value);

    // 2. Update SK
    if (s.IsNotFound()) {
      s = txn_db->Put(write_options, cf_handles[1], current_sk, key);
    } else {
      string new_sk_value = existing_sk_value + ',' + key.ToString();
      s = txn_db->Put(write_options, cf_handles[1], current_sk, new_sk_value);
    }

    // 2. Update PK index
    s = txn_db->Put(write_options, cf_handles[0], key, value);

    // 4. Commit transaction
    s = txn->Commit();
    assert(s.ok());
    delete txn;

    return Status::OK();
  };
  Status Get(const Slice& key, std::string* value) {
    ReadOptions read_options;
    return s = txn_db->Get(read_options, cf_handles[0], key, value);
  };

  vector<Status> GetBySecondaryIndex(const Slice& key,
                                     std::vector<PinnableSlice>* values) {
    ReadOptions read_options;

    // 1. Get PK list by SK
    string pk_list_str;
    s = txn_db->Get(read_options, cf_handles[1], key, &pk_list_str);
    vector<Slice> pk_list;
    // split pk_list by ','
    stringstream ss(pk_list_str);
    string str;
    while (getline(ss, str, ',')) {
      pk_list.push_back(str);
    }

    // 2. Get actual KVPairs by PK
    vector<Status> statuses(pk_list.size());
    values->resize(pk_list.size());
    txn_db->MultiGet(read_options, cf_handles[0], pk_list.size(),
                     pk_list.data(), values->data(), statuses.data());

    return statuses;
  };
};