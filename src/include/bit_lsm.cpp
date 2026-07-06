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
  rocksdb_options_.table_factory.reset(NewBlockBasedTableFactory(opts));

  ColumnFamilyOptions cf_opts(rocksdb_options_);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
  Status s =
      DB::Open(rocksdb_options_, db_path, column_families, &cf_handles_, &db_);
  if (!s.ok()) throw std::runtime_error("Failed to open DB: " + s.ToString());
}

BitLSM::~BitLSM() {
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

unique_ptr<BitLSMIterator> BitLSM::NewIterator(BitLSMQuery& query) {
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

  return std::make_unique<BitLSMIterator>(db_,
                                          cf_handles_[0],  // Default CF
                                          bit_lsm_options_, query);
}

void BitLSM::Statistics() {
  // Get levelstats
  string stats;
  db_->GetProperty("rocksdb.levelstats", &stats);
  cout << stats << "\n";
}