#include "bit_lsm.h"
#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include <chrono>
#include <cstdint>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
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

struct ReadResult {
  uint64_t time_elapsed_ms;
  uint64_t records_matched;
  double selectivity_actual;
};

static vector<uint32_t> parse_indices(const string& s) {
  vector<uint32_t> result;
  stringstream ss(s);
  string token;
  while (getline(ss, token, ','))
    result.push_back(stoul(token));
  return result;
}

static string format_double(double v) {
  ostringstream oss;
  oss << v;
  return oss.str();
}

static BitLSMQuery build_read_query(const BitLSMOptions& options,
                                    const vector<uint32_t>& query_indices,
                                    double selectivity) {
  mt19937 gen(42);
  BitLSMQuery query;
  for (uint32_t idx : query_indices) {
    if (options.attr_types[idx] == AttrType::CONTINUOUS) {
      double width = selectivity * 100.0;
      uniform_real_distribution<double> dist(0.0, 100.0 - width);
      double lo = dist(gen);
      query.conditions.push_back({idx, CompareOp::GREATER_EQUAL, lo});
      query.conditions.push_back({idx, CompareOp::LESS, lo + width});
    } else {
      uniform_int_distribution<int> dist(0, 99);
      query.conditions.push_back({idx, CompareOp::EQUAL, to_string(dist(gen))});
    }
  }
  return query;
}

static ReadResult vanila_full_scan(rocksdb::DB* db,
                                   const BitLSMOptions& options,
                                   BitLSMQuery& query) {
  ReadOptions ro;
  auto* it = db->NewIterator(ro);
  uint64_t matched = 0;
  uint64_t total = 0;
  auto start = chrono::high_resolution_clock::now();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    total++;
    if (query.CheckCondition(it->value(), options))
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

static void save_read_csv(const string& output_dir, const string& exp_label,
                          uint64_t n, uint32_t payload_size, uint32_t attr_num,
                          double selectivity,
                          const string& query_attr_indices_str,
                          const ReadResult& result) {
  filesystem::create_directories(output_dir);
  string filename = exp_label + "_vanila_n" + to_string(n) + "_p" +
                    to_string(payload_size) + "_a" + to_string(attr_num) +
                    "_selectivity" + format_double(selectivity) +
                    "_query_attr_indices" + query_attr_indices_str + ".csv";
  string full_path = output_dir + "/" + filename;
  ofstream f(full_path);
  f << "time_elapsed_ms,records_matched,selectivity_actual\n";
  f << result.time_elapsed_ms << "," << result.records_matched << ","
    << result.selectivity_actual << "\n";
  f.close();
  cout << "Result saved to: " << full_path << "\n";
}

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
                        "Vanilla RocksDB sequential write/read experiment");
  // clang-format off
  opts.add_options()
    ("h,help", "Print usage")
    ("exp_label", "Experiment label for output filename", cxxopts::value<string>()->default_value("seq_write"))
    ("exp_type", "Experiment type: write_seq or read_seq", cxxopts::value<string>()->default_value("write_seq"))
    ("n", "Total records to write", cxxopts::value<uint64_t>())
    ("p,payload_size", "Payload size in bytes", cxxopts::value<uint32_t>()->default_value("32"))
    ("a,attr_num", "Number of attributes per record", cxxopts::value<uint32_t>()->default_value("16"))
    ("d,db_path", "RocksDB storage path", cxxopts::value<string>())
    ("o,output_dir", "Result output directory", cxxopts::value<string>()->default_value("./result"))
    ("selectivity", "Per-attr selectivity (for continuous attrs in read mode)", cxxopts::value<double>())
    ("query_attr_indices", "Comma-separated attr indices for query (e.g. 0,1,3)", cxxopts::value<string>());
  // clang-format on

  auto result = opts.parse(argc, argv);
  if (result.count("help")) {
    cout << opts.help() << "\n";
    return 0;
  }

  string exp_label = result["exp_label"].as<string>();
  string exp_type = result["exp_type"].as<string>();
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

  bool read_mode = (exp_type == "read_seq");

  if (read_mode) {
    // --- Read mode: full scan with condition check ---
    string qi_str = result["query_attr_indices"].as<string>();
    vector<uint32_t> query_indices = parse_indices(qi_str);

    for (uint32_t idx : query_indices) {
      if (idx >= attr_num) {
        cout << "SKIP: query attr index " << idx << " >= attr_num " << attr_num
             << ", skipping experiment.\n";
        return 0;
      }
    }

    // selectivity is required only when continuous attrs are in the query
    double selectivity = 0.0;
    bool has_continuous = false;
    for (uint32_t idx : query_indices) {
      if (options.attr_types[idx] == AttrType::CONTINUOUS) {
        has_continuous = true;
        break;
      }
    }
    if (has_continuous) {
      if (result.count("selectivity") == 0) {
        cerr << "ERROR: --selectivity required when querying continuous attrs\n";
        return 1;
      }
      selectivity = result["selectivity"].as<double>();
    }

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
    vector<ColumnFamilyHandle*> cf_handles;
    Status s = DB::OpenForReadOnly(rocksdb_options, db_path, column_families,
                                   &cf_handles, &db);
    if (!s.ok()) {
      cerr << "Failed to open DB: " << s.ToString() << "\n";
      return 1;
    }

    BitLSMQuery query = build_read_query(options, query_indices, selectivity);
    ReadResult rr = vanila_full_scan(db, options, query);

    for (auto* h : cf_handles)
      db->DestroyColumnFamilyHandle(h);
    delete db;
    cout << "DB successfully closed\n";

    save_read_csv(output_dir, exp_label, n, payload, attr_num, selectivity,
                  qi_str, rr);
  } else {
    // --- Write mode ---
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
    const vector<ColumnFamilyDescriptor> column_families(
        {ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_opts)});
    vector<ColumnFamilyHandle*> cf_handles;
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
  }

  return 0;
}
