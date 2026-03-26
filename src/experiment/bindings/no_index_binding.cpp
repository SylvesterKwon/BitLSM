#include "no_index_binding.h"
#include "bit_lsm_utils.h"
#include <chrono>
#include <iostream>
#include <rocksdb/table.h>

using namespace rocksdb;

namespace experiment {

void NoIndexBinding::Open(int, char*[], const std::string& db_path,
                           const BitLSMOptions& opts) {
  options_ = opts;

  Options rocksdb_options;
  rocksdb_options.create_if_missing = true;
  rocksdb_options.max_background_jobs = 6;
  rocksdb_options.bytes_per_sync = 1048576;
  rocksdb_options.compaction_pri = kMinOverlappingRatio;
  rocksdb_options.max_write_buffer_number = 5;
  BlockBasedTableOptions table_options;
  table_options.block_size = 4 * 1024;
  rocksdb_options.table_factory.reset(
      NewBlockBasedTableFactory(table_options));

  ColumnFamilyOptions cf_opts(rocksdb_options);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const std::vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});

  Status s = DB::Open(rocksdb_options, db_path, column_families,
                      &cf_handles_, &db_);
  if (!s.ok()) {
    std::cerr << "Failed to open DB: " << s.ToString() << "\n";
    exit(1);
  }
}

void NoIndexBinding::Put(const std::string& pk,
                          const std::vector<Attr>& attrs,
                          const std::string& payload) {
  EncodeValue(options_, attrs, payload, serialized_value_);
  db_->Put(wo_, pk, serialized_value_);
}

ScanResult NoIndexBinding::Scan(BitLSMQuery& query) {
  ReadOptions ro;
  auto* it = db_->NewIterator(ro);
  uint64_t matched = 0;
  uint64_t total = 0;
  auto start = std::chrono::high_resolution_clock::now();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    total++;
    if (query.CheckCondition(it->value(), options_))
      matched++;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  delete it;
  std::cout << "scan done: " << matched << "/" << total << " matched, "
            << elapsed << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched};
}

void NoIndexBinding::Close() {
  if (!db_) return;
  if (!cf_handles_.empty()) {
    for (auto* h : cf_handles_)
      db_->DestroyColumnFamilyHandle(h);
    cf_handles_.clear();
  }
  WaitForCompactOptions wait_opts;
  wait_opts.close_db = true;
  db_->WaitForCompact(wait_opts);
  delete db_;
  db_ = nullptr;
  std::cout << "DB successfully closed\n";
}

}  // namespace experiment
