#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "standalone_secondary_index_experiment.h"
#include "util/coding.h"
#include <cstring>

using namespace std;
using namespace rocksdb;

class EagerUpdates : public StandaloneSecondaryIndexExperiment {
private:
  void DecodeIndexValue(Slice& data, vector<Slice>* result) {
    uint32_t total_cnt;
    GetFixed32(&data, &total_cnt);

    result->resize(total_cnt);
    for (Slice& ri : *result) {
      GetLengthPrefixedSlice(&data, &ri);
    }
  }

  void EncodeIndexValue(const vector<Slice>* input, string* dest) {
    PutFixed32(dest, static_cast<uint32_t>(input->size()));

    for (Slice const& i : *input) {
      PutLengthPrefixedSlice(dest, i);
    }
  }

  void InsertSIValue(vector<Slice>* si_value, const Slice& key) {
    auto it = lower_bound(
        si_value->begin(), si_value->end(), key,
        [](const Slice& a, const Slice& b) { return a.compare(b) < 0; });
    if (it == si_value->end()) {
      si_value->push_back(key);
      return;
    } else if (key.compare(*it) == 0) { // found same key
      return;
    } else {
      size_t pos = distance(si_value->begin(), it);
      si_value->push_back(Slice());
      Slice* si_value_data = si_value->data();
      size_t move_count = si_value->size() - 1 - pos;

      memmove(&si_value_data[pos + 1], &si_value_data[pos],
              move_count * sizeof(Slice));
      si_value_data[pos] = key;
      return;
    }
  }

public:
  Status Insert(const Slice& key, const Slice& value) {
    ReadOptions read_options;
    WriteOptions write_options;
    Transaction* txn = txn_db->BeginTransaction(write_options);
    string_view current_sk =
        value.ToStringView().substr(0, value.ToStringView().find(','));

    // 1. Find existing SK entry
    string existing_si_value_str; // {sk_i, {pks...}}
    s = txn_db->Get(read_options, cf_handles[1], current_sk,
                    &existing_si_value_str);

    // 2. Update SK
    if (s.IsNotFound()) {
      vector<Slice> new_si_value = {key};
      string encoded_si_value;
      EncodeIndexValue(&new_si_value, &encoded_si_value);
      txn_db->Put(write_options, cf_handles[1], current_sk, encoded_si_value);
    } else {
      vector<Slice> si_value;
      Slice existing_si_value_slice(existing_si_value_str);
      DecodeIndexValue(existing_si_value_slice, &si_value);

      InsertSIValue(&si_value, key);

      string encoded_si_value;
      EncodeIndexValue(&si_value, &encoded_si_value);
      s = txn_db->Put(write_options, cf_handles[1], current_sk,
                      encoded_si_value);
    }

    // 2. Update PK index
    s = txn_db->Put(write_options, cf_handles[0], key, value);

    // 4. Commit transaction
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