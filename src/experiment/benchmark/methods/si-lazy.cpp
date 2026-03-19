#include "si_benchmark_common.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

class SILazyExperiment
    : public benchmark::BenchmarkExperiment<SILazyExperiment> {
  benchmark::SIDBHandles db_;
  BitLSMOptions options_;
  benchmark::SIStrategy strategy_ = benchmark::SIStrategy::kIndexMerge;
  WriteOptions wo_;
  string serialized_value_;
  // Reusable buffers for Put() hot path
  vector<Slice> single_pk_vec_{1};
  string encoded_si_value_;

 public:
  SILazyExperiment() { method_name = "si-lazy"; }

  void Open(int argc, char* argv[], const string& db_path,
            const Schema& schema, bool) {
    options_ = schema.options;

    cxxopts::Options opts("si-lazy", "");
    opts.allow_unrecognised_options();
    opts.add_options()("read_strategy", "index_merge or post_filter",
                       cxxopts::value<string>()->default_value("index_merge"));
    auto result = opts.parse(argc, argv);
    strategy_ = benchmark::ParseSIStrategy(
        result["read_strategy"].as<string>());
    this->read_param_suffix =
        "_strategy_" + benchmark::SIStrategyToString(strategy_);

    // Open TransactionDB with merge operator on secondary CF
    ColumnFamilyOptions si_cf_opts;
    si_cf_opts.merge_operator.reset(new benchmark::SIValueMergeOperator());
    db_ = benchmark::OpenSITransactionDB(db_path, si_cf_opts);
  }

  void Put(const string& pk, const vector<Attr>& attrs,
           const string& payload) {
    EncodeValue(options_, attrs, payload, serialized_value_);
    Transaction* txn = db_.txn_db->BeginTransaction(wo_);

    // Merge SI entries for each attribute
    for (uint32_t attr_idx = 0; attr_idx < options_.attr_num; ++attr_idx) {
      string si_key;
      if (options_.attr_types[attr_idx] == AttrType::CATEGORICAL) {
        const string& sk_value = get<string>(attrs[attr_idx]);
        si_key = GetInternalSIKey(attr_idx, sk_value);
      } else {
        double sk_value = get<double>(attrs[attr_idx]);
        si_key = GetInternalSIKey(attr_idx, sk_value);
      }

      single_pk_vec_[0] = Slice(pk);
      encoded_si_value_.clear();
      EncodeIndexValue(&single_pk_vec_, &encoded_si_value_);

      txn->Merge(db_.cf_handles[1], si_key, encoded_si_value_);
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
        // Continuous range scan
        ReadOptions ro;
        auto* iter = db_.txn_db->NewIterator(ro, db_.cf_handles[1]);

        string seek_key;
        if (lookup.lower_bound.has_value()) {
          seek_key = GetInternalSIKey(lookup.attr_idx, *lookup.lower_bound);
        } else {
          seek_key = GetInternalSIKey(lookup.attr_idx, rocksdb::Slice(""));
        }

        string prefix = std::to_string(lookup.attr_idx);
        prefix.resize(4, ' ');

        vector<string> all_pks;
        for (iter->Seek(seek_key); iter->Valid(); iter->Next()) {
          auto key = iter->key();
          if (key.size() < 4 + 8 ||
              key.ToStringView().substr(0, 4) != prefix)
            break;

          double key_val = DecodeDoubleOrderPreserving(key.data() + 4);

          if (lookup.lower_bound.has_value()) {
            if (lookup.lower_inclusive) {
              if (key_val < *lookup.lower_bound) continue;
            } else {
              if (key_val <= *lookup.lower_bound) continue;
            }
          }

          if (lookup.upper_bound.has_value()) {
            if (lookup.upper_inclusive) {
              if (key_val > *lookup.upper_bound) break;
            } else {
              if (key_val >= *lookup.upper_bound) break;
            }
          }

          vector<Slice> si_value;
          Slice val_slice = iter->value();
          DecodeIndexValue(val_slice, &si_value);
          for (auto& pk_slice : si_value)
            all_pks.push_back(pk_slice.ToString());
        }
        delete iter;

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
  return SILazyExperiment{}.Run(argc, argv);
}
