#include "si_benchmark_common.h"
#include <algorithm>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

class SIEagerExperiment
    : public benchmark::BenchmarkExperiment<SIEagerExperiment> {
  benchmark::SIDBHandles db_;
  BitLSMOptions options_;
  benchmark::SIStrategy strategy_ = benchmark::SIStrategy::kIndexMerge;
  WriteOptions wo_;
  string serialized_value_;
  // Reusable buffers for Put() hot path
  vector<Slice> si_value_vec_;
  string encoded_si_value_;
  string existing_si_str_;
  string double_key_buf_;  // 8-byte reusable buffer for double encoding

  void InsertSIValue(vector<Slice>* si_value, const Slice& key) {
    auto it = lower_bound(
        si_value->begin(), si_value->end(), key,
        [](const Slice& a, const Slice& b) { return a.compare(b) < 0; });
    if (it != si_value->end() && key.compare(*it) == 0) return;
    size_t pos = distance(si_value->begin(), it);
    si_value->push_back(Slice());
    Slice* data = si_value->data();
    size_t move_count = si_value->size() - 1 - pos;
    if (move_count > 0)
      memmove(&data[pos + 1], &data[pos], move_count * sizeof(Slice));
    data[pos] = key;
  }

 public:
  SIEagerExperiment() { method_name = "si-eager"; }

  void Open(int argc, char* argv[], const string& db_path,
            const Schema& schema, bool) {
    options_ = schema.options;

    cxxopts::Options opts("si-eager", "");
    opts.allow_unrecognised_options();
    opts.add_options()("read_strategy", "index_merge or post_filter",
                       cxxopts::value<string>()->default_value("index_merge"));
    auto result = opts.parse(argc, argv);
    strategy_ = benchmark::ParseSIStrategy(
        result["read_strategy"].as<string>());
    method_param_suffix =
        "_strategy_" + benchmark::SIStrategyToString(strategy_);

    db_ = benchmark::OpenSITransactionDB(db_path);
  }

  void Put(const string& pk, const vector<Attr>& attrs,
           const string& payload) {
    EncodeValue(options_, attrs, payload, serialized_value_);
    Transaction* txn = db_.txn_db->BeginTransaction(wo_);

    ReadOptions ro;
    for (uint32_t attr_idx = 0; attr_idx < options_.attr_num; ++attr_idx) {
      string si_key;
      if (options_.attr_types[attr_idx] == AttrType::CATEGORICAL) {
        const string& sk_value = get<string>(attrs[attr_idx]);
        si_key = GetInternalSIKey(attr_idx, sk_value);
      } else {
        double sk_value = get<double>(attrs[attr_idx]);
        si_key = GetInternalSIKey(attr_idx, sk_value);
      }

      existing_si_str_.clear();
      auto s = txn->Get(ro, db_.cf_handles[1], si_key, &existing_si_str_);

      if (s.IsNotFound()) {
        si_value_vec_.clear();
        si_value_vec_.push_back(Slice(pk));
        encoded_si_value_.clear();
        EncodeIndexValue(&si_value_vec_, &encoded_si_value_);
        txn->Put(db_.cf_handles[1], si_key, encoded_si_value_);
      } else {
        si_value_vec_.clear();
        Slice si_slice(existing_si_str_);
        DecodeIndexValue(si_slice, &si_value_vec_);
        InsertSIValue(&si_value_vec_, Slice(pk));
        encoded_si_value_.clear();
        EncodeIndexValue(&si_value_vec_, &encoded_si_value_);
        txn->Put(db_.cf_handles[1], si_key, encoded_si_value_);
      }
    }

    txn->Put(db_.cf_handles[0], pk, serialized_value_);
    txn->Commit();
    delete txn;
  }

  benchmark::ReadResult Scan(BitLSMQuery& query, uint64_t n) {
    auto plan = benchmark::MapQueryToSILookups(query, options_);

    if (plan.si_lookups.empty())
      return benchmark::ScanFullTable(db_.txn_db, db_.cf_handles,
                                      query, options_, n);

    auto GetPKList = [this](const benchmark::SILookup& lookup) -> vector<string> {
      if (lookup.type == benchmark::SILookupType::kPointLookup) {
        // Categorical equality: point Get
        ReadOptions ro;
        string si_value_str;
        auto s = db_.txn_db->Get(ro, db_.cf_handles[1],
                                  GetInternalSIKey(lookup.attr_idx, lookup.sk_value),
                                  &si_value_str);
        if (s.IsNotFound()) return {};

        vector<Slice> si_value;
        Slice si_slice(si_value_str);
        DecodeIndexValue(si_slice, &si_value);

        vector<string> result(si_value.size());
        for (size_t i = 0; i < si_value.size(); ++i)
          result[i] = si_value[i].ToString();
        return result;
      } else {
        // Continuous range scan: iterator over SI entries
        ReadOptions ro;
        auto* iter = db_.txn_db->NewIterator(ro, db_.cf_handles[1]);

        // Build seek key from lower bound
        string seek_key;
        if (lookup.lower_bound.has_value()) {
          seek_key = GetInternalSIKey(lookup.attr_idx, *lookup.lower_bound);
        } else {
          // No lower bound: seek to start of this attr's SI prefix
          seek_key = GetInternalSIKey(lookup.attr_idx, rocksdb::Slice(""));
        }

        // idx_no prefix for boundary checks
        string prefix = std::to_string(lookup.attr_idx);
        prefix.resize(4, ' ');

        vector<string> all_pks;
        for (iter->Seek(seek_key); iter->Valid(); iter->Next()) {
          auto key = iter->key();
          // Check we're still in the same attr's SI space
          if (key.size() < 4 + 8 ||
              key.ToStringView().substr(0, 4) != prefix)
            break;

          double key_val = DecodeDoubleOrderPreserving(key.data() + 4);

          // Check lower bound
          if (lookup.lower_bound.has_value()) {
            if (lookup.lower_inclusive) {
              if (key_val < *lookup.lower_bound) continue;
            } else {
              if (key_val <= *lookup.lower_bound) continue;
            }
          }

          // Check upper bound
          if (lookup.upper_bound.has_value()) {
            if (lookup.upper_inclusive) {
              if (key_val > *lookup.upper_bound) break;
            } else {
              if (key_val >= *lookup.upper_bound) break;
            }
          }

          // Decode PK list from this entry's value
          vector<Slice> si_value;
          Slice val_slice = iter->value();
          DecodeIndexValue(val_slice, &si_value);
          for (auto& pk_slice : si_value)
            all_pks.push_back(pk_slice.ToString());
        }
        delete iter;

        // Sort for set_intersection in IndexMerge
        std::sort(all_pks.begin(), all_pks.end());
        return all_pks;
      }
    };

    if (strategy_ == benchmark::SIStrategy::kIndexMerge) {
      return benchmark::ScanByIndexMerge(db_.txn_db, db_.cf_handles, plan,
                                          options_, n, GetPKList);
    } else {
      return benchmark::ScanByPostFiltering(db_.txn_db, db_.cf_handles, plan,
                                             query, options_, n, GetPKList);
    }
  }

  void Close() { benchmark::CloseSITransactionDB(db_); }
};

int main(const int argc, char* argv[]) {
  return SIEagerExperiment{}.Run(argc, argv);
}
