#include "bit_lsm.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

rocksdb::DB* db;
bit_lsm::BitLSMOptions options;
atomic<uint64_t> global_auto_increment(0);
chrono::_V2::system_clock::time_point start_time;

inline const char* char_set =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
inline const size_t max_index = strlen(char_set) - 1;

inline string random_string(size_t length) {
  string res;
  res.reserve(length);
  for (size_t i = 0; i < length; ++i)
    res += char_set[rand() % max_index];
  return res;
}

struct ProgressLog {
  uint64_t time_elapsed_ms;
  uint64_t records_written;
};
vector<ProgressLog> progress_log;
mutex progress_log_mutex;

void put_thread_worker(uint32_t thread_id, uint64_t total_n,
                       uint32_t payload_size) {
  WriteOptions wo;
  mt19937 gen(thread_id);
  uniform_int_distribution<int> cat_dist(0, 99);
  uniform_real_distribution<double> cont_dist(0.0, 100.0);
  uniform_int_distribution<size_t> char_dist(0, max_index);

  while (true) {
    uint64_t current_pk_val = global_auto_increment++;
    if (current_pk_val >= total_n)
      break;

    vector<Attr> attrs(options.attr_num);
    for (uint32_t j = 0; j < options.attr_num; ++j) {
      if (j % 2 == 0)
        attrs[j] = to_string(cat_dist(gen));
      else
        attrs[j] = cont_dist(gen);
    }

    string payload;
    payload.reserve(payload_size);
    for (size_t i = 0; i < payload_size; ++i)
      payload += char_set[char_dist(gen)];
    string pk = to_string(current_pk_val);

    string serialized_value;
    EncodeValue(options, attrs, payload, serialized_value);
    db->Put(wo, pk, serialized_value);

    if ((current_pk_val + 1) % 1000000 == 0) {
      auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(
                            chrono::high_resolution_clock::now() - start_time)
                            .count();
      {
        lock_guard<mutex> lock(progress_log_mutex);
        progress_log.push_back(
            {static_cast<uint64_t>(elapsed_ms), current_pk_val + 1});
      }
      cout << "putted: " << current_pk_val + 1
           << " kvps, elapsed: " << elapsed_ms << "ms\n";
    }
  }
}

void vanila_fill_kvp_using_sequential(uint32_t num_threads, uint64_t n,
                                      uint32_t payload_size = 32,
                                      bool debug = false) {
  if (debug)
    cout << "creating " << n << " kvps into Vanila using Put API...\n";
  global_auto_increment = 0;
  start_time = chrono::high_resolution_clock::now();
  vector<thread> workers;
  for (int i = 0; i < num_threads; ++i)
    workers.emplace_back(put_thread_worker, i, n, payload_size);
  for (auto& t : workers)
    t.join();
  if (debug) {
    cout << "✅ created " << n << " kvps. (total:"
         << chrono::duration_cast<chrono::milliseconds>(
                chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms elapsed)\n";
  }
}

void save_csv(const string& output_dir, const string& exp_type, uint64_t n,
              uint32_t payload_size, uint32_t num_threads, uint32_t attr_num) {
  filesystem::create_directories(output_dir);
  string filename = exp_type + "_vanila_n" + to_string(n) + "_p" +
                    to_string(payload_size) + "_t" + to_string(num_threads) +
                    "_a" + to_string(attr_num) + ".csv";
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
    ("exp_type", "Experiment type", cxxopts::value<string>()->default_value("seq_write"))
    ("n", "Total records to write", cxxopts::value<uint64_t>())
    ("t,threads", "Number of writer threads", cxxopts::value<uint32_t>()->default_value("4"))
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

  string exp_type = result["exp_type"].as<string>();
  uint64_t n = result["n"].as<uint64_t>();
  uint32_t n_threads = result["threads"].as<uint32_t>();
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
  BlockBasedTableOptions table_options;
  table_options.block_size = 16 * 1024;
  table_options.cache_index_and_filter_blocks = true;
  table_options.pin_l0_filter_and_index_blocks_in_cache = true;
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

  vanila_fill_kvp_using_sequential(n_threads, n, payload, true);

  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db->WaitForCompact(wait_for_compact_options);
  assert(s.ok());
  delete db;
  cout << "DB successfully closed\n";

  save_csv(output_dir, exp_type, n, payload, n_threads, attr_num);

  return 0;
}
