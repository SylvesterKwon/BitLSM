#include "bit_lsm.h"
#include "bit_lsm_utils.h"
#include "schema_loader.h"
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
using namespace roaring;
using namespace bit_lsm;

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

static string schema_stem(const string& path) {
  auto pos = path.find_last_of('/');
  string fname = (pos == string::npos) ? path : path.substr(pos + 1);
  auto dot = fname.find_last_of('.');
  return (dot == string::npos) ? fname : fname.substr(0, dot);
}

static BitLSMQuery build_read_query(const Schema& schema,
                                    const vector<uint32_t>& query_indices,
                                    double selectivity) {
  mt19937 gen(42);
  BitLSMQuery query;
  for (uint32_t idx : query_indices) {
    if (schema.options.attr_types[idx] == AttrType::CONTINUOUS) {
      double range = schema.range_max[idx] - schema.range_min[idx];
      double width = selectivity * range;
      uniform_real_distribution<double> dist(schema.range_min[idx],
                                             schema.range_max[idx] - width);
      double lo = dist(gen);
      query.conditions.push_back({idx, CompareOp::GREATER_EQUAL, lo});
      query.conditions.push_back({idx, CompareOp::LESS, lo + width});
    } else {
      uniform_int_distribution<int> dist(0, schema.cardinalities[idx] - 1);
      query.conditions.push_back({idx, CompareOp::EQUAL, to_string(dist(gen))});
    }
  }
  return query;
}

static ReadResult bitlsm_scan(BitLSM& db, BitLSMQuery& query, uint64_t n) {
  uint64_t matched = 0;
  auto start = chrono::high_resolution_clock::now();
  auto iter = db.NewIterator(query);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    matched++;
  auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                     chrono::high_resolution_clock::now() - start)
                     .count();
  cout << "scan done: " << matched << "/" << n << " matched, " << elapsed
       << "ms\n";
  return {static_cast<uint64_t>(elapsed), matched,
          n > 0 ? static_cast<double>(matched) / n : 0.0};
}

static void fill_kvp_with_log(bit_lsm::BitLSM* db, uint64_t n,
                              const Schema& schema,
                              vector<ProgressLog>& progress_log,
                              bool debug = false) {
  if (debug)
    cout << "creating " << n << " kvps into BitLSM using Put API...\n";

  mt19937 gen(42);
  uniform_int_distribution<size_t> char_dist(0, max_index);

  // Per-attr distributions
  vector<uniform_int_distribution<int>> cat_dists;
  vector<uniform_real_distribution<double>> cont_dists;
  for (uint32_t j = 0; j < schema.options.attr_num; ++j) {
    if (schema.options.attr_types[j] == AttrType::CATEGORICAL)
      cat_dists.emplace_back(0, schema.cardinalities[j] - 1);
    else
      cat_dists.emplace_back(0, 0);
    if (schema.options.attr_types[j] == AttrType::CONTINUOUS)
      cont_dists.emplace_back(schema.range_min[j], schema.range_max[j]);
    else
      cont_dists.emplace_back(0.0, 0.0);
  }

  auto start_time = chrono::high_resolution_clock::now();

  for (uint64_t i = 1; i <= n; ++i) {
    vector<Attr> attrs(schema.options.attr_num);
    for (uint32_t j = 0; j < schema.options.attr_num; ++j) {
      if (schema.options.attr_types[j] == AttrType::CATEGORICAL)
        attrs[j] = to_string(cat_dists[j](gen));
      else
        attrs[j] = cont_dists[j](gen);
    }

    string payload;
    payload.reserve(schema.payload_bytes);
    for (size_t k = 0; k < schema.payload_bytes; ++k)
      payload += char_set[char_dist(gen)];

    string pk;
    pk.reserve(8);
    for (size_t k = 0; k < 8; ++k)
      pk += char_set[char_dist(gen)];

    db->Put(pk, attrs, payload);

    if (i % 1000000 == 0) {
      auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(
                            chrono::high_resolution_clock::now() - start_time)
                            .count();
      progress_log.push_back({static_cast<uint64_t>(elapsed_ms), i});
      cout << "putted: " << i << " kvps, elapsed: " << elapsed_ms << "ms\n";
    }
  }

  if (debug) {
    cout << "created " << n << " kvps. (total:"
         << chrono::duration_cast<chrono::milliseconds>(
                chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms elapsed)\n";
  }
}

static void save_csv(const string& output_dir, const string& exp_label,
                     uint64_t n, const string& schema_name, double rho,
                     const vector<ProgressLog>& progress_log) {
  filesystem::create_directories(output_dir);
  string filename = exp_label + "_bitlsm_n" + to_string(n) + "_schema_" +
                    schema_name + "_rho" + format_double(rho) + ".csv";
  string full_path = output_dir + "/" + filename;
  ofstream f(full_path);
  f << "time_elapsed_ms,records_written\n";
  for (auto& log : progress_log)
    f << log.time_elapsed_ms << "," << log.records_written << "\n";
  f.close();
  cout << "Result saved to: " << full_path << "\n";
}

static void save_read_csv(const string& output_dir, const string& exp_label,
                          uint64_t n, const string& schema_name, double rho,
                          double selectivity,
                          const string& query_attr_indices_str,
                          const ReadResult& result) {
  filesystem::create_directories(output_dir);
  string filename = exp_label + "_bitlsm_n" + to_string(n) + "_schema_" +
                    schema_name + "_rho" + format_double(rho) +
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

int main(const int argc, char* argv[]) {
  cxxopts::Options opts("bit-lsm", "BitLSM sequential write/read experiment");
  // clang-format off
  opts.add_options()
    ("h,help", "Print usage")
    ("exp_label", "Experiment label for output filename", cxxopts::value<string>()->default_value("seq_write"))
    ("exp_type", "Experiment type: write_seq or read_seq", cxxopts::value<string>()->default_value("write_seq"))
    ("n", "Total records to write", cxxopts::value<uint64_t>())
    ("schema", "Path to schema JSON file", cxxopts::value<string>())
    ("rho", "BitLSM rho threshold", cxxopts::value<double>()->default_value("0.1"))
    ("d,db_path", "BitLSM storage path", cxxopts::value<string>())
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
  string schema_path = result["schema"].as<string>();
  double rho = result["rho"].as<double>();
  string db_path = result["db_path"].as<string>();
  string output_dir = result["output_dir"].as<string>();

  Schema schema = load_schema(schema_path);
  string s_name = schema_stem(schema_path);
  schema.options.rho = rho;

  bool read_mode = (exp_type == "read_seq");

  if (read_mode) {
    // --- Read mode: bitmap-indexed scan ---
    string qi_str = result["query_attr_indices"].as<string>();
    vector<uint32_t> query_indices = parse_indices(qi_str);

    for (uint32_t idx : query_indices) {
      if (idx >= schema.options.attr_num) {
        cout << "SKIP: query attr index " << idx << " >= attr_num "
             << schema.options.attr_num << ", skipping experiment.\n";
        return 0;
      }
    }

    // selectivity is required only when continuous attrs are in the query
    double selectivity = 0.0;
    bool has_continuous = false;
    for (uint32_t idx : query_indices) {
      if (schema.options.attr_types[idx] == AttrType::CONTINUOUS) {
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

    BitLSM db(db_path, schema.options);
    BitLSMQuery query = build_read_query(schema, query_indices, selectivity);
    ReadResult rr = bitlsm_scan(db, query, n);
    cout << "RESULT:" << rr.time_elapsed_ms << "," << rr.records_matched << ","
         << rr.selectivity_actual << "\n";
  } else {
    // --- Write mode ---
    BitLSM db(db_path, schema.options);
    vector<ProgressLog> progress_log;
    fill_kvp_with_log(&db, n, schema, progress_log, true);
    save_csv(output_dir, exp_label, n, s_name, rho, progress_log);
  }

  return 0;
}
