#include "rocksdb/merge_operator.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "standalone_secondary_index_experiment.h"
#include "standalone_secondary_index_utils.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

using namespace std;
using namespace rocksdb;

class SIValueMergeOperator : public MergeOperator {
public:
  const char* Name() const override { return "SIValueMergeOperator"; }

  bool FullMergeV2(const MergeOperationInput& merge_in,
                   MergeOperationOutput* merge_out) const override {
    vector<Slice> all_operand_list = merge_in.operand_list;
    if (merge_in.existing_value)
      all_operand_list.push_back(*merge_in.existing_value);
    MergeIndexValue(all_operand_list, &merge_out->new_value);

    return true;
  }

  // For performance when merging in memtable
  bool PartialMergeMulti(const Slice& key, const deque<Slice>& operand_list,
                         string* new_value, Logger* logger) const override {
    vector<Slice> all_operand_list(operand_list.begin(), operand_list.end());
    MergeIndexValue(all_operand_list, new_value);
    return true;
  }
};

class LazyUpdates : public StandaloneSecondaryIndexExperiment {
protected:
  void ConfigureCustomDBOptions() override {
    column_families[1].options.merge_operator.reset(new SIValueMergeOperator());
  }

public:
  vector<CompositeQueryRunStrategy>
  GetAvailableCompositeQueryRunStrategy() const override {
    return {kIndexMerge, kPostFiltering};
  };

  Status Insert(const Slice& key, const Slice& value) override {
    ReadOptions read_options;
    WriteOptions write_options;
    Transaction* txn = txn_db->BeginTransaction(write_options);

    // 1. Update SK index
    string current_sk;
    stringstream ss(value.ToString());
    for (uint32_t idx_no = 0; idx_no < si_cnt; ++idx_no) {
      getline(ss, current_sk, ',');
      vector<Slice> new_si_value = {key};
      string encoded_si_value;
      EncodeIndexValue(&new_si_value, &encoded_si_value);
      s = txn->Merge(cf_handles[1], GetInternalSIKey(idx_no, current_sk),
                     encoded_si_value);
      assert(s.ok());
    }

    // 2. Update PK index
    s = txn->Put(cf_handles[0], key, value);
    assert(s.ok());

    // 3. Commit transaction
    s = txn->Commit();
    assert(s.ok());
    delete txn;

    return Status::OK();
  };

  Status GetBySecondaryIndex(const uint32_t idx_no, const Slice& key,
                             vector<pair<string, string>>* results) override {
    ReadOptions read_options;

    // 1. Get PK list by SK
    string existing_si_value_str;
    s = txn_db->Get(read_options, cf_handles[1], GetInternalSIKey(idx_no, key),
                    &existing_si_value_str);
    if (s.IsNotFound())
      return {};

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
      (*results)[i] = {si_value[i].ToString(), values[i].ToString()};
    }

    return Status::OK();
  };

  Status GetBySecondaryIndices(const vector<pair<uint32_t, Slice>>& query,
                               CompositeQueryRunStrategy strategy,
                               vector<pair<string, string>>* results) override {
    if (strategy == CompositeQueryRunStrategy::kIndexMerge) {
      return GetByIndexMerge(query, results);
    } else if (strategy == CompositeQueryRunStrategy::kPostFiltering) {
      return GetByTableAccessByIndexPK(query, results);
    } else
      return Status::NotSupported("Not supported composited query strategy.");
    return Status::OK();
  };

  // GetByIndexMerge for Lazy Updates, Eager Updates
  Status GetByIndexMerge(const vector<pair<uint32_t, Slice>>& query,
                         vector<pair<string, string>>* results) {
    ReadOptions read_options;

    // 1. Get PK list by SK
    bool is_first_query = true;
    vector<string> merged_result_str;
    for (auto& [idx_no, key] : query) {
      string existing_si_value_str;
      s = txn_db->Get(read_options, cf_handles[1],
                      GetInternalSIKey(idx_no, key), &existing_si_value_str);
      if (s.IsNotFound())
        return {};

      vector<Slice> si_value;
      Slice existing_si_value_slice(existing_si_value_str);
      DecodeIndexValue(existing_si_value_slice, &si_value);

      vector<string> si_value_str(si_value.size()), tmp;
      for (uint32_t i = 0; i < si_value.size(); ++i)
        si_value_str[i] = si_value[i].ToString();

      if (is_first_query) {
        merged_result_str.swap(si_value_str);
        is_first_query = false;
      } else {
        set_intersection(merged_result_str.begin(), merged_result_str.end(),
                         si_value_str.begin(), si_value_str.end(),
                         back_inserter(tmp));
        merged_result_str.swap(tmp);
      }
      if (merged_result_str.size() == 0)
        return {};
    }
    vector<Slice> merged_result(merged_result_str.size());
    for (uint32_t i = 0; i < merged_result_str.size(); ++i)
      merged_result[i] = Slice(merged_result_str[i]);

    // 2. Get actual KVPairs by PK
    size_t result_size = merged_result_str.size();
    vector<Status> statuses(result_size);
    results->resize(result_size);
    vector<PinnableSlice> values(result_size);
    txn_db->MultiGet(read_options, cf_handles[0], result_size,
                     merged_result.data(), values.data(), statuses.data());

    // 3. Construct result KVPairs
    for (size_t i = 0; i < result_size; ++i) {
      assert(statuses[i].ok());
      (*results)[i] = {merged_result_str[i], values[i].ToString()};
    }

    return Status::OK();
  };

  Status GetByTableAccessByIndexPK(const vector<pair<uint32_t, Slice>>& query,
                                   vector<pair<string, string>>* results) {
    GetBySecondaryIndex(query[0].first, query[0].second, results);
    erase_if(*results, [&](const pair<string, string>& x) {
      for (uint32_t q_no = 1; q_no < query.size(); ++q_no) {
        auto& idx_no = query[q_no].first;
        auto& key = query[q_no].second;
        string_view target_sk = GetIthToken(x.second, idx_no, ',');
        if (key.compare(target_sk))
          return true;
      }
      return false;
    });
    return Status::OK();
  };
};