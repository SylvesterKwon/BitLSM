#include "bit_lsm.h"
#include "bit_lsm_iterator.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "rocksdb/options.h"
#include "sabi.h"
#include <iostream>
#include <string>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

BitLSM::BitLSM(const string& db_path, const BitLSMOptions& bit_lsm_options)
    : db_path_(db_path), bit_lsm_options_(bit_lsm_options) {
  // configure DB
  rocksdb_options_.create_if_missing = true;
  BlockBasedTableOptions table_options;
  table_options.user_defined_index_factory =
      make_shared<SABIFactory>(bit_lsm_options_);
  rocksdb_options_.table_factory.reset(
      NewBlockBasedTableFactory(table_options));
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, rocksdb_options_)});
  Status s =
      DB::Open(rocksdb_options_, db_path, column_families, &cf_handles_, &db_);
  if (!s.ok())
    cerr << "Failed to open DB: " << s.ToString() << "\n";
  assert(s.ok());
}

BitLSM::~BitLSM() {
  Status s;
  // Close DB gracefully
  for (auto handle : cf_handles_)
    db_->DestroyColumnFamilyHandle(handle);
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db_->WaitForCompact(wait_for_compact_options);
  assert(s.ok());
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

  // 2. Serialize value
  string serialized_value;
  EncodeValue(bit_lsm_options_, attrs, payload, serialized_value);

  // 3. Put Key-Value pair to RocksDB
  return db_->Put(WriteOptions(), key, serialized_value);
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
    EncodeValue(bit_lsm_options_, attrs_list[i], payloads[i], serialized_value);
    batch.Put(pks[i], serialized_value);
  }
  rocksdb::WriteOptions wo;
  return db_->Write(wo, &batch);
}

Status BitLSM::Delete(const string& key) {
  return db_->Delete(WriteOptions(), key);
}

unique_ptr<BitLSMIterator> BitLSM::NewIterator(BitLSMQuery& query) {
  // Sort query condition by attr_idx
  std::sort(query.conditions.begin(), query.conditions.end(),
            [](const QueryCondition& a, const QueryCondition& b) {
              return a.attr_idx < b.attr_idx;
            });

  return std::make_unique<BitLSMIterator>(
      db_,
      cf_handles_[0], // 기본 Column Family 사용
      bit_lsm_options_, query);
}

void BitLSM::Statistics() {
  // Get levelstats
  string stats;
  db_->GetProperty("rocksdb.levelstats", &stats);
  cout << stats << "\n";
}