#include "bit_lsm.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "bit_lsm_iterator.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "block_prefetch_queue.h"
#include "rocksdb/options.h"
#include "sabi.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

namespace {
// Per-CF rocksdb options: shared base + this CF's schema-bound SABI factory.
rocksdb::ColumnFamilyOptions BuildCFOptions(
    const rocksdb::Options& base, const BlockBasedTableOptions& table_options,
    const BitLSMOptions& options) {
  rocksdb::ColumnFamilyOptions cf_opts(base);
  BlockBasedTableOptions opts = table_options;
  opts.user_defined_index_factory = make_shared<SABIFactory>(options);
  cf_opts.table_factory.reset(NewBlockBasedTableFactory(opts));
  cf_opts.level_compaction_dynamic_level_bytes = true;
  return cf_opts;
}
}  // namespace

BitLSM::BitLSM(const string& db_path, const BitLSMOptions& bit_lsm_options,
               const Options& rocksdb_options,
               const BlockBasedTableOptions& table_options)
    : BitLSM(db_path, rocksdb_options, table_options,
             {{rocksdb::kDefaultColumnFamilyName, bit_lsm_options}}) {}

BitLSM::BitLSM(const string& db_path, const Options& rocksdb_options,
               const BlockBasedTableOptions& table_options,
               const vector<ColumnFamilyDescriptor>& descriptors)
    : db_path_(db_path),
      rocksdb_options_(rocksdb_options),
      table_options_(table_options) {
  bool has_default = false;
  bool want_uring = false;
  for (const auto& d : descriptors) {
    if (d.name == rocksdb::kDefaultColumnFamilyName) has_default = true;
    if (d.options.scan_prefetch_depth > 0 || d.options.ondemand_index)
      want_uring = true;
  }
  if (!has_default)
    throw std::invalid_argument(
        "descriptors must include the default column family");

  // Before Open: RocksDB decides whether a file gets io_uring rings when the
  // file is opened, so async reads -- the scan prefetch queue's, and in
  // on-demand mode the SABI span prefetch's -- have to be opted into now or
  // never (block_prefetch_queue.h). A CF created later with those options
  // when no descriptor wanted them here degrades to synchronous reads.
  if (want_uring) EnableRocksDbIOUring();

  // Registered before Open (the listener list is frozen there); estimators
  // arm per cf id as their CFs come up.
  stats_listener_ = make_shared<StatsRefreshListener>();
  rocksdb_options_.listeners.push_back(stats_listener_);

  vector<rocksdb::ColumnFamilyDescriptor> cf_descs;
  for (const auto& d : descriptors)
    cf_descs.emplace_back(
        d.name, BuildCFOptions(rocksdb_options_, table_options_, d.options));

  vector<rocksdb::ColumnFamilyHandle*> handles;
  Status s = DB::Open(rocksdb_options_, db_path, cf_descs, &handles, &db_);
  if (!s.ok()) throw std::runtime_error("Failed to open DB: " + s.ToString());

  for (size_t i = 0; i < descriptors.size(); ++i)
    RegisterColumnFamily(descriptors[i].name, handles[i],
                         descriptors[i].options);
  default_cf_ = cf_registry_.at(rocksdb::kDefaultColumnFamilyName).get();
}

bit_lsm::ColumnFamilyHandle* BitLSM::RegisterColumnFamily(
    const string& name, rocksdb::ColumnFamilyHandle* handle,
    const BitLSMOptions& options) {
  auto cf = std::unique_ptr<ColumnFamilyHandle>(
      new ColumnFamilyHandle(name, handle, options));
  if (options.enable_estimator) {
    cf->estimator_ = make_unique<CardinalityEstimator>(
        static_cast<DBImpl*>(db_),
        static_cast<ColumnFamilyHandleImpl*>(handle)->cfd(),
        SABISchema::FromOptions(options), options);
    stats_listener_->Arm(handle->GetID(), cf->estimator_.get());
  }
  ColumnFamilyHandle* raw = cf.get();
  std::lock_guard<std::mutex> lock(cf_mu_);
  cf_registry_.emplace(name, std::move(cf));
  return raw;
}

BitLSM::~BitLSM() {
  // Stop stats refresh before the DB goes away: the workers reference their
  // column families, and close-time flushes must not signal dead estimators.
  if (stats_listener_) stats_listener_->DisarmAll();
  for (auto& [name, cf] : cf_registry_) {
    cf->estimator_.reset();
    db_->DestroyColumnFamilyHandle(cf->rocksdb_handle_);
  }

  Status s;
  // Close DB gracefully
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db_->WaitForCompact(wait_for_compact_options);
  if (!s.ok()) cerr << "Failed to close DB: " << s.ToString() << "\n";
  delete db_;
  cout << "DB successfully closed\n";
}

bit_lsm::ColumnFamilyHandle* BitLSM::GetColumnFamily(const string& name) const {
  std::lock_guard<std::mutex> lock(cf_mu_);
  auto it = cf_registry_.find(name);
  return it != cf_registry_.end() ? it->second.get() : nullptr;
}

Status BitLSM::CreateColumnFamily(const string& name,
                                  const BitLSMOptions& options,
                                  ColumnFamilyHandle** out) {
  {
    std::lock_guard<std::mutex> lock(cf_mu_);
    if (cf_registry_.count(name))
      return Status::InvalidArgument("column family already exists: " + name);
  }
  rocksdb::ColumnFamilyHandle* handle = nullptr;
  Status s = db_->CreateColumnFamily(
      BuildCFOptions(rocksdb_options_, table_options_, options), name, &handle);
  if (!s.ok()) return s;
  *out = RegisterColumnFamily(name, handle, options);
  return Status::OK();
}

Status BitLSM::DropColumnFamily(ColumnFamilyHandle* cf) {
  if (cf == nullptr || cf == default_cf_)
    return Status::InvalidArgument(
        "cannot drop null or the default column family");
  // Stop stats refresh first: the worker references the CF being dropped.
  stats_listener_->Disarm(cf->id());
  cf->estimator_.reset();
  Status s = db_->DropColumnFamily(cf->rocksdb_handle_);
  if (!s.ok()) return s;
  db_->DestroyColumnFamilyHandle(cf->rocksdb_handle_);
  std::lock_guard<std::mutex> lock(cf_mu_);
  cf_registry_.erase(cf->name());
  return Status::OK();
}

Status BitLSM::ListColumnFamilies(const Options& options, const string& db_path,
                                  vector<string>* out) {
  return DB::ListColumnFamilies(rocksdb::DBOptions(options), db_path, out);
}

Status BitLSM::Put(ColumnFamilyHandle* cf, const string& pk,
                   const vector<Attr>& attrs, const string& payload) {
  // 1. Validate # of indexed attrs
  if (attrs.size() != cf->options().attr_num) {
    return Status::InvalidArgument(
        "The number of attrs does not match with db configuration.");
  }

  // 2. Serialize value (thread_local for concurrent Put safety)
  thread_local string serialized_value_buf;
  EncodeValue(cf->layout(), attrs, payload, serialized_value_buf);

  // 3. Put Key-Value pair to RocksDB
  return db_->Put(WriteOptions(), cf->rocksdb_handle(), pk,
                  serialized_value_buf);
}

Status BitLSM::PutBatch(ColumnFamilyHandle* cf, const vector<string>& pks,
                        const vector<vector<Attr>>& attrs_list,
                        const vector<string>& payloads) {
  // Assume all given vector has same length
  WriteBatch batch;
  uint32_t batch_size = pks.size();

  for (uint32_t i = 0; i < batch_size; ++i) {
    if (attrs_list[i].size() != cf->options().attr_num) {
      return Status::InvalidArgument(
          "PutBatch error: attrs size at index " + to_string(i) +
          " does not match the configured attr_num.");
    }

    string serialized_value;
    EncodeValue(cf->layout(), attrs_list[i], payloads[i], serialized_value);
    batch.Put(cf->rocksdb_handle(), pks[i], serialized_value);
  }
  WriteOptions wo;
  return db_->Write(wo, &batch);
}

Status BitLSM::Delete(ColumnFamilyHandle* cf, const string& key) {
  return db_->Delete(WriteOptions(), cf->rocksdb_handle(), key);
}

Status BitLSM::Flush(ColumnFamilyHandle* cf) {
  return db_->Flush(FlushOptions(), cf->rocksdb_handle());
}

unique_ptr<BitLSMIterator> BitLSM::NewIterator(ColumnFamilyHandle* cf,
                                               BitLSMQuery& query,
                                               ResultMode result_mode,
                                               const Snapshot* snapshot) {
  // Invalid queries (see BitLSMQuery::Validate) get nullptr; callers needing
  // the reason call Validate directly.
  if (!query.Validate(cf->options()).ok()) return nullptr;

  // Sort conditions within each OR clause by attr_idx so same-attr conditions
  // are adjacent — guarantees CheckCondition's per-clause decode cache hits.
  for (auto& clause : query.clause_groups) {
    std::sort(clause.begin(), clause.end(),
              [](const QueryCondition& a, const QueryCondition& b) {
                return a.attr_idx < b.attr_idx;
              });
  }

  return std::make_unique<BitLSMIterator>(
      db_, cf->rocksdb_handle(), cf->options(), query, result_mode, snapshot);
}

void BitLSM::Statistics() {
  // Get levelstats
  string stats;
  db_->GetProperty("rocksdb.levelstats", &stats);
  cout << stats << "\n";
}
