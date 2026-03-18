#include "si_benchmark_common.h"
#include <algorithm>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

class SIEagerExperiment
    : public benchmark::BenchmarkExperiment<SIEagerExperiment> {
  benchmark::SIDBHandles db_;
  BitLSMOptions options_;
  benchmark::SIMapping si_mapping_;
  benchmark::SIStrategy strategy_ = benchmark::SIStrategy::kIndexMerge;
  WriteOptions wo_;
  string serialized_value_;
  // Reusable buffers for Put() hot path
  vector<Slice> si_value_vec_;
  string encoded_si_value_;
  string existing_si_str_;

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
    si_mapping_ = benchmark::BuildSIMapping(schema);

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
    for (uint32_t si_no = 0; si_no < si_mapping_.si_cnt; ++si_no) {
      uint32_t attr_idx = si_mapping_.si_to_attr[si_no];
      const string& sk_value = get<string>(attrs[attr_idx]);
      string si_key = GetInternalSIKey(si_no, sk_value);

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
    auto plan = benchmark::MapQueryToSILookups(query, si_mapping_, options_);

    if (plan.si_lookups.empty())
      return benchmark::ScanFullTable(db_.txn_db, db_.cf_handles,
                                      query, options_, n);

    auto GetPKList = [this](uint32_t si_no,
                            const string& sk) -> vector<string> {
      ReadOptions ro;
      string si_value_str;
      auto s = db_.txn_db->Get(ro, db_.cf_handles[1],
                                GetInternalSIKey(si_no, sk), &si_value_str);
      if (s.IsNotFound()) return {};

      vector<Slice> si_value;
      Slice si_slice(si_value_str);
      DecodeIndexValue(si_slice, &si_value);

      vector<string> result(si_value.size());
      for (size_t i = 0; i < si_value.size(); ++i)
        result[i] = si_value[i].ToString();
      return result;
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
