#include "bit_lsm.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include <chrono>
#include <cstdint>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

rocksdb::DB* db;
bit_lsm::BitLSMOptions options;

inline const char* char_set =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
inline const size_t max_index = strlen(char_set) - 1;

struct ProgressLog {
  uint64_t time_elapsed_ms;
  uint64_t records_written;
};

static void vanila_fill_kvp(uint64_t n, uint32_t payload_size,
                            vector<ProgressLog>& progress_log,
                            bool debug = false) {
  if (debug)
    cout << "creating " << n << " kvps into Vanila using Put API...\n";

  WriteOptions wo;
  mt19937 gen(42);
  uniform_int_distribution<int> cat_dist(0, 99);
  uniform_real_distribution<double> cont_dist(0.0, 100.0);
  uniform_int_distribution<size_t> char_dist(0, max_index);

  auto start_time = chrono::high_resolution_clock::now();

  for (uint64_t i = 0; i < n; ++i) {
    vector<Attr> attrs(options.attr_num);
    for (uint32_t j = 0; j < options.attr_num; ++j) {
      if (j % 2 == 0)
        attrs[j] = to_string(cat_dist(gen));
      else
        attrs[j] = cont_dist(gen);
    }

    string payload;
    payload.reserve(payload_size);
    for (size_t k = 0; k < payload_size; ++k)
      payload += char_set[char_dist(gen)];

    string pk;
    pk.reserve(8);
    for (size_t k = 0; k < 8; ++k)
      pk += char_set[char_dist(gen)];

    string serialized_value;
    EncodeValue(options, attrs, payload, serialized_value);
    db->Put(wo, pk, serialized_value);

    if ((i + 1) % 1000000 == 0) {
      auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(
                            chrono::high_resolution_clock::now() - start_time)
                            .count();
      progress_log.push_back({static_cast<uint64_t>(elapsed_ms), i + 1});
      cout << "putted: " << i + 1 << " kvps, elapsed: " << elapsed_ms << "ms\n";
    }
  }

  if (debug) {
    cout << "✅ created " << n << " kvps. (total:"
         << chrono::duration_cast<chrono::milliseconds>(
                chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms elapsed)\n";
  }
}

static void save_csv(const string& output_dir, const string& exp_label,
                     uint64_t n, uint32_t payload_size, uint32_t attr_num,
                     const vector<ProgressLog>& progress_log) {
  filesystem::create_directories(output_dir);
  string filename = exp_label + "_vanila_n" + to_string(n) + "_p" +
                    to_string(payload_size) + "_a" + to_string(attr_num) +
                    ".csv";
  string full_path = output_dir + "/" + filename;
  ofstream f(full_path);
  f << "time_elapsed_ms,records_written\n";
  for (auto& log : progress_log)
    f << log.time_elapsed_ms << "," << log.records_written << "\n";
  f.close();
  cout << "Result saved to: " << full_path << "\n";
}

int main(const int argc, char* argv[]) {
  cxxopts::Options opts("vanila-rocksdb",
                        "Vanilla RocksDB sequential write experiment");
  // clang-format off
  opts.add_options()
    ("h,help", "Print usage")
    ("exp_label", "Experiment label for output filename", cxxopts::value<string>()->default_value("seq_write"))
    ("n", "Total records to write", cxxopts::value<uint64_t>())
    ("p,payload_size", "Payload size in bytes", cxxopts::value<uint32_t>()->default_value("32"))
    ("a,attr_num", "Number of attributes per record", cxxopts::value<uint32_t>()->default_value("16"))
    ("d,db_path", "RocksDB storage path", cxxopts::value<string>())
    ("o,output_dir", "Result output directory", cxxopts::value<string>()->default_value("./result"));
  // clang-format on

  auto result = opts.parse(argc, argv);
  if (result.count("help")) {
    cout << opts.help() << "\n";
    return 0;
  }

  string exp_label = result["exp_label"].as<string>();
  uint64_t n = result["n"].as<uint64_t>();
  uint32_t payload = result["payload_size"].as<uint32_t>();
  uint32_t attr_num = result["attr_num"].as<uint32_t>();
  string db_path = result["db_path"].as<string>();
  string output_dir = result["output_dir"].as<string>();

  options.attr_num = attr_num;
  for (uint32_t i = 0; i < options.attr_num; ++i) {
    if (i % 2 == 0)
      options.attr_types.push_back(AttrType::CATEGORICAL);
    else
      options.attr_types.push_back(AttrType::CONTINUOUS);
  }

  // configure DB
  // Using recommending options for better performance:
  // (reference:
  // https://github.com/facebook/rocksdb/wiki/Setup-Options-and-Basic-Tuning)
  Options rocksdb_options;
  rocksdb_options.create_if_missing = true;
  rocksdb_options.max_background_jobs = 6;
  rocksdb_options.bytes_per_sync = 1048576;
  rocksdb_options.compaction_pri = kMinOverlappingRatio;
  // reference:
  // https://github.com/facebook/rocksdb/wiki/RocksDB-Tuning-Guide#flushing-options
  // rocksdb_options.write_buffer_size = 64 << 20; // Default: 64MB
  rocksdb_options.max_write_buffer_number = 5;
  // rocksdb_options.min_write_buffer_number_to_merge = 2;
  BlockBasedTableOptions table_options;
  table_options.block_size = 4 * 1024; // default, 4kb
  rocksdb_options.table_factory.reset(NewBlockBasedTableFactory(table_options));
  ColumnFamilyOptions cf_opts(rocksdb_options);
  cf_opts.level_compaction_dynamic_level_bytes = true;
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
  Status s =
      DB::Open(rocksdb_options, db_path, column_families, &cf_handles, &db);
  if (!s.ok()) {
    cerr << "Failed to open DB: " << s.ToString() << "\n";
    return 1;
  }

  vector<ProgressLog> progress_log;
  vanila_fill_kvp(n, payload, progress_log, true);

  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db->WaitForCompact(wait_for_compact_options);
  assert(s.ok());
  delete db;
  cout << "DB successfully closed\n";

  save_csv(output_dir, exp_label, n, payload, attr_num, progress_log);

  return 0;
}
