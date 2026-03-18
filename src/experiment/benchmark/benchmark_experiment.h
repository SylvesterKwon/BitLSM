#pragma once

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include "schema_loader.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace benchmark {

using std::cerr;
using std::cout;
using std::mt19937;
using std::ofstream;
using std::string;
using std::to_string;
using std::uniform_int_distribution;
using std::uniform_real_distribution;
using std::vector;
using bit_lsm::AttrType;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;

inline const char* kCharSet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
inline const size_t kMaxCharIndex = strlen(kCharSet) - 1;

struct ProgressLog {
  uint64_t time_elapsed_ms;
  uint64_t records_written;
};

struct ReadResult {
  uint64_t time_elapsed_ms;
  uint64_t records_matched;
  double selectivity_actual;
};

inline vector<uint32_t> parse_indices(const string& s) {
  vector<uint32_t> result;
  std::stringstream ss(s);
  string token;
  while (getline(ss, token, ','))
    result.push_back(stoul(token));
  return result;
}

inline string format_double(double v) {
  std::ostringstream oss;
  oss << v;
  return oss.str();
}

inline string schema_stem(const string& path) {
  auto pos = path.find_last_of('/');
  string fname = (pos == string::npos) ? path : path.substr(pos + 1);
  auto dot = fname.find_last_of('.');
  return (dot == string::npos) ? fname : fname.substr(0, dot);
}

inline BitLSMQuery build_read_query(const Schema& schema,
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

// CRTP base class for benchmark experiments.
// Derived must implement: Open, Put, Scan, Close
// Derived must set: method_name, method_param_suffix (in constructor or Open)
template <typename Derived>
class BenchmarkExperiment {
 public:
  string method_name;
  string method_param_suffix;

  int Run(int argc, char* argv[]) {
    cxxopts::Options opts(self().method_name,
                          self().method_name + " benchmark experiment");
    opts.allow_unrecognised_options();
    // clang-format off
    opts.add_options()
      ("h,help", "Print usage")
      ("exp_label", "Experiment label", cxxopts::value<string>()->default_value("seq_write"))
      ("exp_type", "write_seq or read_seq", cxxopts::value<string>()->default_value("write_seq"))
      ("n", "Total records", cxxopts::value<uint64_t>())
      ("schema", "Schema JSON path", cxxopts::value<string>())
      ("d,db_path", "DB storage path", cxxopts::value<string>())
      ("o,output_dir", "Output directory", cxxopts::value<string>()->default_value("./result"))
      ("selectivity", "Per-attr selectivity", cxxopts::value<double>())
      ("query_attr_indices", "Comma-separated attr indices", cxxopts::value<string>());
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
    string db_path = result["db_path"].as<string>();
    string output_dir = result["output_dir"].as<string>();

    Schema schema = load_schema(schema_path);
    string s_name = schema_stem(schema_path);
    bool read_mode = (exp_type == "read_seq");

    self().Open(argc, argv, db_path, schema, read_mode);

    if (read_mode) {
      string qi_str = result["query_attr_indices"].as<string>();
      vector<uint32_t> query_indices = parse_indices(qi_str);

      for (uint32_t idx : query_indices) {
        if (idx >= schema.options.attr_num) {
          cout << "SKIP: query attr index " << idx << " >= attr_num "
               << schema.options.attr_num << ", skipping experiment.\n";
          self().Close();
          return 0;
        }
      }

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
          cerr << "ERROR: --selectivity required when querying continuous "
                  "attrs\n";
          self().Close();
          return 1;
        }
        selectivity = result["selectivity"].as<double>();
      }

      BitLSMQuery query = build_read_query(schema, query_indices, selectivity);
      ReadResult rr = self().Scan(query, n);
      self().Close();

      cout << "RESULT:" << rr.time_elapsed_ms << "," << rr.records_matched
           << "," << rr.selectivity_actual << "\n";
    } else {
      vector<ProgressLog> progress_log;
      FillKVP(schema, n, progress_log);
      self().Close();
      SaveWriteCSV(output_dir, exp_label, n, s_name, progress_log);
    }

    return 0;
  }

 protected:
  Derived& self() { return static_cast<Derived&>(*this); }

  void FillKVP(const Schema& schema, uint64_t n,
               vector<ProgressLog>& progress_log) {
    cout << "creating " << n << " kvps into " << method_name
         << " using Put API...\n";

    mt19937 gen(42);
    uniform_int_distribution<size_t> char_dist(0, kMaxCharIndex);

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

    auto start_time = std::chrono::high_resolution_clock::now();

    vector<Attr> attrs(schema.options.attr_num);
    string payload(schema.payload_bytes, '\0');
    string pk(8, '\0');

    for (uint64_t i = 0; i < n; ++i) {
      for (uint32_t j = 0; j < schema.options.attr_num; ++j) {
        if (schema.options.attr_types[j] == AttrType::CATEGORICAL)
          attrs[j] = to_string(cat_dists[j](gen));
        else
          attrs[j] = cont_dists[j](gen);
      }

      for (size_t k = 0; k < schema.payload_bytes; ++k)
        payload[k] = kCharSet[char_dist(gen)];

      for (size_t k = 0; k < 8; ++k)
        pk[k] = kCharSet[char_dist(gen)];

      self().Put(pk, attrs, payload);

      if ((i + 1) % 1000000 == 0) {
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start_time)
                .count();
        progress_log.push_back({static_cast<uint64_t>(elapsed_ms), i + 1});
        cout << "putted: " << i + 1 << " kvps, elapsed: " << elapsed_ms
             << "ms\n";
      }
    }

    cout << "created " << n << " kvps. (total:"
         << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms elapsed)\n";
  }

  void SaveWriteCSV(const string& output_dir, const string& exp_label,
                    uint64_t n, const string& schema_name,
                    const vector<ProgressLog>& progress_log) {
    std::filesystem::create_directories(output_dir);
    string filename = exp_label + "_" + self().method_name + "_n" +
                      to_string(n) + "_schema_" + schema_name +
                      self().method_param_suffix + ".csv";
    string full_path = output_dir + "/" + filename;
    ofstream f(full_path);
    f << "time_elapsed_ms,records_written\n";
    for (auto& log : progress_log)
      f << log.time_elapsed_ms << "," << log.records_written << "\n";
    f.close();
    cout << "Result saved to: " << full_path << "\n";
  }
};

}  // namespace benchmark
