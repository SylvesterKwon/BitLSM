#include "bit_lsm.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "bit_lsm_iterator.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "rocksdb/options.h"
#include "sabi.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

BitLSM::BitLSM(const string& db_path, const BitLSMOptions& bit_lsm_options,
               const Options& rocksdb_options,
               const BlockBasedTableOptions& table_options)
    : db_path_(db_path),
      bit_lsm_options_(bit_lsm_options),
      layout_(bit_lsm_options) {
  rocksdb_options_ = rocksdb_options;

  BlockBasedTableOptions opts = table_options;
  opts.user_defined_index_factory = make_shared<SABIFactory>(bit_lsm_options_);
  if (bit_lsm_options_.ondemand_index) {
    // RocksDB reads and parses the blob once when it opens a table, and with
    // cache_index_and_filter_blocks off it then pins that entry in the table
    // reader for the file's lifetime -- every bin resident, which is exactly
    // what reading bins on demand exists to avoid. Caching the entry instead
    // lets it age out, after which nothing re-fetches it: scans go through the
    // directory registry and the blob source.
    opts.cache_index_and_filter_blocks = true;
    // Directories and blob pages are keyed by file number, so an entry that
    // outlived its file would serve another file's bytes.
    rocksdb_options_.listeners.push_back(
        make_shared<SABIRegistryCleaner>(&sabi_registry_));
  }
  rocksdb_options_.table_factory.reset(NewBlockBasedTableFactory(opts));
  // Read back what the factory settled on: with no explicit block_cache it
  // creates one, and that is the cache the blob pages must share.
  block_cache_ =
      rocksdb_options_.table_factory->GetOptions<BlockBasedTableOptions>()
          ->block_cache;

  // Registered before Open, armed only once the estimator exists.
  if (bit_lsm_options_.enable_estimator) {
    stats_listener_ = make_shared<StatsRefreshListener>();
    rocksdb_options_.listeners.push_back(stats_listener_);
  }

  ColumnFamilyOptions cf_opts(rocksdb_options_);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
  Status s =
      DB::Open(rocksdb_options_, db_path, column_families, &cf_handles_, &db_);
  if (!s.ok()) throw std::runtime_error("Failed to open DB: " + s.ToString());

  if (bit_lsm_options_.enable_estimator) {
    estimator_ = make_unique<CardinalityEstimator>(
        static_cast<DBImpl*>(db_),
        static_cast<ColumnFamilyHandleImpl*>(cf_handles_[0])->cfd(),
        SABISchema::FromOptions(bit_lsm_options_), bit_lsm_options_);
    stats_listener_->Arm(estimator_.get());
  }
}

BitLSM::~BitLSM() {
  // Stop stats refresh before the DB goes away: the worker references the
  // column family, and close-time flushes must not signal a dead estimator.
  if (stats_listener_) stats_listener_->Disarm();
  estimator_.reset();

  Status s;
  // Close DB gracefully
  for (auto handle : cf_handles_) db_->DestroyColumnFamilyHandle(handle);
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db_->WaitForCompact(wait_for_compact_options);
  if (!s.ok()) cerr << "Failed to close DB: " << s.ToString() << "\n";
  delete db_;
  cout << "DB successfully closed\n";
}

Status BitLSM::Put(const string& key, const vector<Attr>& attrs,
                   const string& payload) {
  // 1. Validate # of indexed attrs
  if (attrs.size() != bit_lsm_options_.attr_num) {
    return Status::InvalidArgument(
        "The number of attrs does not match with db configuration.");
  }

  // 2. Serialize value (thread_local for concurrent Put safety)
  thread_local string serialized_value_buf;
  EncodeValue(layout_, attrs, payload, serialized_value_buf);

  // 3. Put Key-Value pair to RocksDB
  return db_->Put(WriteOptions(), key, serialized_value_buf);
}

rocksdb::Status BitLSM::PutBatch(const vector<string>& pks,
                                 const vector<vector<Attr>>& attrs_list,
                                 const vector<string>& payloads) {
  // Assume all given vector has same length
  WriteBatch batch;
  uint32_t batch_size = pks.size();

  for (uint32_t i = 0; i < batch_size; ++i) {
    if (attrs_list[i].size() != bit_lsm_options_.attr_num) {
      return rocksdb::Status::InvalidArgument(
          "PutBatch error: attrs size at index " + to_string(i) +
          " does not match the configured attr_num.");
    }

    string serialized_value;
    EncodeValue(layout_, attrs_list[i], payloads[i], serialized_value);
    batch.Put(pks[i], serialized_value);
  }
  rocksdb::WriteOptions wo;
  return db_->Write(wo, &batch);
}

Status BitLSM::Delete(const string& key) {
  return db_->Delete(WriteOptions(), key);
}

Status BitLSM::Flush() { return db_->Flush(FlushOptions(), cf_handles_[0]); }

unique_ptr<BitLSMIterator> BitLSM::NewIterator(BitLSMQuery& query,
                                               ColumnFamilyHandle* cfh,
                                               ResultMode result_mode,
                                               const Snapshot* snapshot) {
  // Invalid queries (see BitLSMQuery::Validate) get nullptr; callers needing
  // the reason call Validate directly.
  if (!query.Validate(bit_lsm_options_).ok()) return nullptr;

  // Sort conditions within each OR clause by attr_idx so same-attr conditions
  // are adjacent — guarantees CheckCondition's per-clause decode cache hits.
  for (auto& clause : query.clause_groups) {
    std::sort(clause.begin(), clause.end(),
              [](const QueryCondition& a, const QueryCondition& b) {
                return a.attr_idx < b.attr_idx;
              });
  }

  SABIIndexContext index_ctx;
  if (bit_lsm_options_.ondemand_index) {
    index_ctx.registry = &sabi_registry_;
    index_ctx.cache = block_cache_;
  }

  return std::make_unique<BitLSMIterator>(
      db_, cfh != nullptr ? cfh : cf_handles_[0], bit_lsm_options_, query,
      result_mode, snapshot, std::move(index_ctx));
}

void BitLSM::Statistics() {
  // Get levelstats
  string stats;
  db_->GetProperty("rocksdb.levelstats", &stats);
  cout << stats << "\n";
}