#include "si_benchmark_common.h"
#include <rocksdb/filter_policy.h>
#include <rocksdb/slice_transform.h>
#include <algorithm>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

class SICompositeExperiment
    : public benchmark::BenchmarkExperiment<SICompositeExperiment> {
  benchmark::SIDBHandles db_;
  BitLSMOptions options_;
  benchmark::SIMapping si_mapping_;
  benchmark::SIStrategy strategy_ = benchmark::SIStrategy::kIndexMerge;
  WriteOptions wo_;
  string serialized_value_;
  // Composite key parameters (fixed)
  static constexpr uint32_t idx_no_prefix_size_ = 4;
  static constexpr uint32_t si_prefix_length_ = 16;
  // Reusable buffer for Put()
  string si_key_buf_;

 public:
  SICompositeExperiment() { method_name = "si-composite"; }

  void Open(int argc, char* argv[], const string& db_path,
            const Schema& schema, bool) {
    options_ = schema.options;
    si_mapping_ = benchmark::BuildSIMapping(schema);

    cxxopts::Options opts("si-composite", "");
    opts.allow_unrecognised_options();
    opts.add_options()("read_strategy", "index_merge or post_filter",
                       cxxopts::value<string>()->default_value("index_merge"));
    auto result = opts.parse(argc, argv);
    strategy_ = benchmark::ParseSIStrategy(
        result["read_strategy"].as<string>());
    method_param_suffix =
        "_strategy_" + benchmark::SIStrategyToString(strategy_);

    // Open TransactionDB with prefix bloom filter on secondary CF
    ColumnFamilyOptions si_cf_opts;
    BlockBasedTableOptions si_table_options;
    si_table_options.filter_policy.reset(NewBloomFilterPolicy(10, false));
    si_table_options.whole_key_filtering = false;
    si_cf_opts.table_factory.reset(
        NewBlockBasedTableFactory(si_table_options));
    si_cf_opts.prefix_extractor.reset(
        NewCappedPrefixTransform(idx_no_prefix_size_ + si_prefix_length_));

    db_ = benchmark::OpenSITransactionDB(db_path, si_cf_opts);
  }

  void Put(const string& pk, const vector<Attr>& attrs,
           const string& payload) {
    EncodeValue(options_, attrs, payload, serialized_value_);
    Transaction* txn = db_.txn_db->BeginTransaction(wo_);

    for (uint32_t si_no = 0; si_no < si_mapping_.si_cnt; ++si_no) {
      uint32_t attr_idx = si_mapping_.si_to_attr[si_no];
      const string& sk_value = get<string>(attrs[attr_idx]);

      // Build composite key: [idx_no_padded][sk_padded][pk]
      si_key_buf_ = sk_value;
      si_key_buf_.resize(si_prefix_length_, ' ');
      si_key_buf_ += pk;
      txn->Put(db_.cf_handles[1],
               GetInternalSIKey(si_no, si_key_buf_, idx_no_prefix_size_), "");
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
      ReadOptions si_ro;
      si_ro.auto_prefix_mode = true;
      si_ro.prefix_same_as_start = true;
      si_ro.total_order_seek = false;

      string resized_sk = sk;
      resized_sk.resize(si_prefix_length_, ' ');
      string seek_key = GetInternalSIKey(si_no, resized_sk);
      uint32_t prefix_size = idx_no_prefix_size_ + si_prefix_length_;

      auto* it = db_.txn_db->NewIterator(si_ro, db_.cf_handles[1]);
      vector<string> pk_list;
      for (it->Seek(seek_key); it->Valid(); it->Next()) {
        auto key = it->key();
        pk_list.push_back(key.ToString().substr(prefix_size,
                                                 key.size() - prefix_size));
      }
      delete it;
      return pk_list;
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
  return SICompositeExperiment{}.Run(argc, argv);
}
