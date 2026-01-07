#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/table.h"
#include "standalone_secondary_index_experiment.h"
#include "standalone_secondary_index_utils.h"
#include <cstring>
#include <sstream>

using namespace std;
using namespace rocksdb;

class CompositeKeys : public StandaloneSecondaryIndexExperiment {
  // TODO: composite SI 가능하도록 수정하기. 현재는 단일 SK + PK 구조만 지원
private:
  // TODO: make this customizable
  // (must modified before ConfigureCustomDBOptions call)
  uint32_t idx_no_prefix_size = 4;
  uint32_t si_prefix_length = 16; // prefix length of SI entities
  uint32_t prefix_bloom_bits_per_key = 10;

protected:
  void ConfigureCustomDBOptions() override {
    // Set up bloom filter
    // (reference: https://github.com/facebook/rocksdb/wiki/Prefix-Seek)
    BlockBasedTableOptions table_options;
    table_options.filter_policy.reset(
        NewBloomFilterPolicy(prefix_bloom_bits_per_key, false));
    table_options.whole_key_filtering = false;
    column_families[1].options.table_factory.reset(
        NewBlockBasedTableFactory(table_options));
    column_families[1].options.prefix_extractor.reset(
        NewCappedPrefixTransform(idx_no_prefix_size + si_prefix_length));
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

    // 1. Update SK
    string current_sk;
    stringstream ss(value.ToString());
    for (uint32_t idx_no = 0; idx_no < si_cnt; ++idx_no) {
      getline(ss, current_sk, ',');
      assert(current_sk.size() <= si_prefix_length);
      current_sk.resize(si_prefix_length, ' ');
      current_sk += key.ToString();
      txn->Put(cf_handles[1],
               GetInternalSIKey(idx_no, current_sk, idx_no_prefix_size), "");
    }

    // 2. Update PK index
    s = txn->Put(cf_handles[0], key, value);

    // 3. Commit transaction
    s = txn->Commit();
    assert(s.ok());
    delete txn;

    return Status::OK();
  };

  Status GetBySecondaryIndex(const uint32_t idx_no, const Slice& key,
                             vector<pair<string, string>>* results) override {
    ReadOptions si_read_options;
    si_read_options.auto_prefix_mode = true;
    si_read_options.prefix_same_as_start = true;
    si_read_options.total_order_seek = false;

    // 1. Get PK list by SK
    Iterator* it = txn_db->NewIterator(si_read_options, cf_handles[1]);
    string resized_key = key.ToString();
    resized_key.resize(si_prefix_length, ' ');

    string si_key = GetInternalSIKey(idx_no, resized_key);
    vector<string> pk_str_list;
    uint32_t prefix_size = idx_no_prefix_size + si_prefix_length;
    for (it->Seek(si_key); it->Valid(); it->Next()) {
      assert(it->status().ok());
      pk_str_list.push_back(it->key().ToString().substr(
          prefix_size, it->key().size() - prefix_size));
    }
    delete it;
    vector<Slice> pk_slice_list(pk_str_list.size());
    for (uint32_t i = 0; i < pk_str_list.size(); ++i) {
      pk_slice_list[i] = Slice(pk_str_list[i]);
    }

    // 2. Get actual KVPairs by PK
    ReadOptions read_options;
    size_t result_size = pk_slice_list.size();
    vector<Status> statuses(result_size);
    results->resize(result_size);
    vector<PinnableSlice> values(result_size);
    txn_db->MultiGet(read_options, cf_handles[0], result_size,
                     pk_slice_list.data(), values.data(), statuses.data());

    // 3. Construct result KVPairs
    for (size_t i = 0; i < result_size; ++i) {
      assert(statuses[i].ok());
      (*results)[i] = {pk_slice_list[i].ToString(), values[i].ToString()};
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

  Status GetByIndexMerge(const vector<pair<uint32_t, Slice>>& query,
                         vector<pair<string, string>>* results) {
    ReadOptions si_read_options;
    ReadOptions read_options;
    si_read_options.auto_prefix_mode = true;
    si_read_options.prefix_same_as_start = true;
    si_read_options.total_order_seek = false;
    Iterator* it = txn_db->NewIterator(si_read_options, cf_handles[1]);

    bool is_first_query = true;
    vector<string> merged_result_str;
    for (auto& [idx_no, key] : query) {
      string resized_key = key.ToString();
      resized_key.resize(si_prefix_length, ' ');
      string si_key = GetInternalSIKey(idx_no, resized_key);
      vector<string> pk_str_list, tmp;
      uint32_t prefix_size = idx_no_prefix_size + si_prefix_length;
      for (it->Seek(si_key); it->Valid(); it->Next()) {
        assert(it->status().ok());
        pk_str_list.push_back(it->key().ToString().substr(
            prefix_size, it->key().size() - prefix_size));
      }

      if (is_first_query) {
        merged_result_str.swap(pk_str_list);
        is_first_query = false;
      } else {
        set_intersection(merged_result_str.begin(), merged_result_str.end(),
                         pk_str_list.begin(), pk_str_list.end(),
                         back_inserter(tmp));
        merged_result_str.swap(tmp);
      }
    }
    delete it;
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