#include "benchmark_experiment.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

class NoIndexExperiment
    : public benchmark::BenchmarkExperiment<NoIndexExperiment> {
  rocksdb::DB* db_ = nullptr;
  vector<ColumnFamilyHandle*> cf_handles_;
  BitLSMOptions options_;
  WriteOptions wo_;

 public:
  NoIndexExperiment() { method_name = "no-index"; }

  void Open(int, char*[], const string& db_path, const Schema& schema,
            bool read_mode) {
    options_ = schema.options;

    Options rocksdb_options;
    rocksdb_options.max_background_jobs = 6;
    BlockBasedTableOptions table_options;
    table_options.block_size = 4 * 1024;
    rocksdb_options.table_factory.reset(
        NewBlockBasedTableFactory(table_options));
    ColumnFamilyOptions cf_opts(rocksdb_options);
    cf_opts.level_compaction_dynamic_level_bytes = true;
    const vector<ColumnFamilyDescriptor> column_families(
        {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});

    if (read_mode) {
      Status s = DB::OpenForReadOnly(rocksdb_options, db_path, column_families,
                                     &cf_handles_, &db_);
      if (!s.ok()) {
        cerr << "Failed to open DB: " << s.ToString() << "\n";
        exit(1);
      }
    } else {
      rocksdb_options.create_if_missing = true;
      rocksdb_options.bytes_per_sync = 1048576;
      rocksdb_options.compaction_pri = kMinOverlappingRatio;
      rocksdb_options.max_write_buffer_number = 5;
      Status s =
          DB::Open(rocksdb_options, db_path, column_families, &cf_handles_,
                   &db_);
      if (!s.ok()) {
        cerr << "Failed to open DB: " << s.ToString() << "\n";
        exit(1);
      }
    }
  }

  void Put(const string& pk, const vector<Attr>&attrs,
           const string& payload) {
    string serialized_value;
    EncodeValue(options_, attrs, payload, serialized_value);
    db_->Put(wo_, pk, serialized_value);
  }

  benchmark::ReadResult Scan(BitLSMQuery& query, uint64_t) {
    ReadOptions ro;
    auto* it = db_->NewIterator(ro);
    uint64_t matched = 0;
    uint64_t total = 0;
    auto start = chrono::high_resolution_clock::now();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      total++;
      if (query.CheckCondition(it->value(), options_))
        matched++;
    }
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                       chrono::high_resolution_clock::now() - start)
                       .count();
    delete it;
    cout << "scan done: " << matched << "/" << total << " matched, " << elapsed
         << "ms\n";
    return {static_cast<uint64_t>(elapsed), matched,
            total > 0 ? static_cast<double>(matched) / total : 0.0};
  }

  void Close() {
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
    cout << "DB successfully closed\n";
  }
};

int main(const int argc, char* argv[]) {
  return NoIndexExperiment{}.Run(argc, argv);
}
