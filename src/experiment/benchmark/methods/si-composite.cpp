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

    for (uint32_t attr_idx = 0; attr_idx < options_.attr_num; ++attr_idx) {
      if (options_.attr_types[attr_idx] == AttrType::CATEGORICAL) {
        const string& sk_value = get<string>(attrs[attr_idx]);
        // Build composite key: [idx_no_padded][sk_padded][pk]
        si_key_buf_ = sk_value;
        si_key_buf_.resize(si_prefix_length_, ' ');
        si_key_buf_ += pk;
        txn->Put(db_.cf_handles[1],
                 GetInternalSIKey(attr_idx, si_key_buf_, idx_no_prefix_size_),
                 "");
      } else {
        // CONTINUOUS: [idx_no_padded][encoded_double_padded][pk]
        double sk_value = get<double>(attrs[attr_idx]);
        char encoded[8];
        EncodeDoubleOrderPreserving(sk_value, encoded);
        si_key_buf_.assign(encoded, 8);
        si_key_buf_.resize(si_prefix_length_, '\0');  // pad with zero bytes
        si_key_buf_ += pk;
        txn->Put(db_.cf_handles[1],
                 GetInternalSIKey(attr_idx, si_key_buf_, idx_no_prefix_size_),
                 "");
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
        // Categorical: prefix seek (same as before)
        ReadOptions si_ro;
        si_ro.auto_prefix_mode = true;
        si_ro.prefix_same_as_start = true;
        si_ro.total_order_seek = false;

        string resized_sk = lookup.sk_value;
        resized_sk.resize(si_prefix_length_, ' ');
        string seek_key = GetInternalSIKey(lookup.attr_idx, resized_sk);
        uint32_t prefix_size = idx_no_prefix_size_ + si_prefix_length_;

        auto* it = db_.txn_db->NewIterator(si_ro, db_.cf_handles[1]);
        vector<string> pk_list;
        for (it->Seek(seek_key); it->Valid(); it->Next()) {
          auto key = it->key();
          pk_list.push_back(key.ToString().substr(prefix_size,
                                                   key.size() - prefix_size));
        }
        delete it;
        std::sort(pk_list.begin(), pk_list.end());
        return pk_list;
      } else {
        // CONTINUOUS: range scan with total_order_seek
        ReadOptions si_ro;
        si_ro.total_order_seek = true;

        // Build lower-bound seek key
        string seek_key;
        if (lookup.lower_bound.has_value()) {
          char encoded[8];
          EncodeDoubleOrderPreserving(*lookup.lower_bound, encoded);
          string sk_part(encoded, 8);
          sk_part.resize(si_prefix_length_, '\0');
          seek_key = GetInternalSIKey(lookup.attr_idx, sk_part);
        } else {
          string sk_part(si_prefix_length_, '\0');
          seek_key = GetInternalSIKey(lookup.attr_idx, sk_part);
        }

        string prefix = std::to_string(lookup.attr_idx);
        prefix.resize(idx_no_prefix_size_, ' ');

        uint32_t prefix_size = idx_no_prefix_size_ + si_prefix_length_;

        auto* it = db_.txn_db->NewIterator(si_ro, db_.cf_handles[1]);
        vector<string> pk_list;
        for (it->Seek(seek_key); it->Valid(); it->Next()) {
          auto key = it->key();
          if (key.size() < prefix_size ||
              key.ToStringView().substr(0, idx_no_prefix_size_) != prefix)
            break;

          // Extract encoded double from key (bytes 4..12)
          double key_val = DecodeDoubleOrderPreserving(
              key.data() + idx_no_prefix_size_);

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

          // Extract PK from key suffix (after prefix_size)
          pk_list.push_back(
              key.ToString().substr(prefix_size, key.size() - prefix_size));
        }
        delete it;
        std::sort(pk_list.begin(), pk_list.end());
        return pk_list;
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
  return SICompositeExperiment{}.Run(argc, argv);
}
