#include "rocksdb/merge_operator.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "standalone_secondary_index_experiment.h"
#include "standalone_secondary_index_utils.h"
#include <cstring>

using namespace std;
using namespace rocksdb;

class LazyUpdatesSIMergeOperator : public AssociativeMergeOperator {
public:
  virtual bool Merge(const Slice& key, const Slice* existing_value,
                     const Slice& value, std::string* new_value,
                     Logger* logger) const override {
    if (existing_value) {
      vector<Slice> v1;
      Slice tmp_slice = (*existing_value); // to remove const
      DecodeIndexValue(tmp_slice, &v1);
      vector<Slice> v2;
      Slice new_value_slice(value);
      DecodeIndexValue(new_value_slice, &v2);

      // performs merge sort like merging
      std::vector<Slice> new_si_value;
      new_si_value.reserve(v1.size() + v2.size());
      size_t i = 0, j = 0;
      while (i < v1.size() && j < v2.size()) {
        int cmp = v1[i].compare(v2[j]);
        if (cmp < 0) {
          new_si_value.push_back(v1[i++]);
        } else if (cmp > 0) {
          new_si_value.push_back(v2[j++]);
        } else { // remove duplicated values
          // it is guaranteed that one vector has no duplicated value
          new_si_value.push_back(v1[i]);
          i++, j++;
        }
      }
      while (i < v1.size())
        new_si_value.push_back(v1[i++]);
      while (j < v2.size())
        new_si_value.push_back(v2[j++]);

      EncodeIndexValue(&new_si_value, new_value);
    } else {
      new_value->assign(value.data(), value.size());
    }

    return true;
  }

  virtual const char* Name() const override {
    return "LazyUpdatesSIMergeOperator";
  }
};

class LazyUpdates : public StandaloneSecondaryIndexExperiment {
protected:
  void ConfigureCustomDBOptions() override {
    cout << "Setting up Lazy Merge Operator...\n";
    column_families[1].options.merge_operator.reset(
        new LazyUpdatesSIMergeOperator());
  }

public:
  Status Insert(const Slice& key, const Slice& value) {
    ReadOptions read_options;
    WriteOptions write_options;
    Transaction* txn = txn_db->BeginTransaction(write_options);
    string_view current_sk =
        value.ToStringView().substr(0, value.ToStringView().find(','));

    // 1. Update SK index
    vector<Slice> new_si_value = {key};
    string encoded_si_value;
    EncodeIndexValue(&new_si_value, &encoded_si_value);
    s = txn_db->Merge(write_options, cf_handles[1], current_sk,
                      encoded_si_value);
    assert(s.ok());

    // 2. Update PK index
    s = txn_db->Put(write_options, cf_handles[0], key, value);
    assert(s.ok());

    // 3. Commit transaction
    s = txn->Commit();
    assert(s.ok());
    delete txn;

    return Status::OK();
  };

  Status Get(const Slice& key, string* value) {
    ReadOptions read_options;
    return s = txn_db->Get(read_options, cf_handles[0], key, value);
  };

  vector<Status>
  GetBySecondaryIndex(const Slice& key,
                      vector<pair<string, PinnableSlice>>* results) {
    ReadOptions read_options;

    // 1. Get PK list by SK
    string existing_si_value_str;
    s = txn_db->Get(read_options, cf_handles[1], key, &existing_si_value_str);
    vector<Slice> si_value;
    Slice existing_si_value_slice(existing_si_value_str);
    DecodeIndexValue(existing_si_value_slice, &si_value);

    // 2. Get actual KVPairs by PK
    size_t result_size = si_value.size();
    vector<Status> statuses(result_size);
    results->resize(result_size);
    vector<PinnableSlice> values(result_size);
    txn_db->MultiGet(read_options, cf_handles[0], result_size, si_value.data(),
                     values.data(), statuses.data());

    // 3. Construct result KVPairs
    for (size_t i = 0; i < result_size; ++i) {
      assert(statuses[i].ok());
      (*results)[i].first = si_value[i].ToString();
      (*results)[i].second = std::move(values[i]);
    }

    return statuses;
  };
};