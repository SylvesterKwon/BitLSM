#include "bit_lsm.h"
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
using namespace roaring;
using namespace bit_lsm;

inline const char* char_set =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
inline const size_t max_index = strlen(char_set) - 1;

struct ProgressLog {
  uint64_t time_elapsed_ms;
  uint64_t records_written;
};

static void fill_kvp_with_log(bit_lsm::BitLSM* db, uint64_t n,
                              bit_lsm::BitLSMOptions opts,
                              uint32_t payload_size,
                              vector<ProgressLog>& progress_log,
                              bool debug = false) {
  if (debug)
    cout << "creating " << n << " kvps into BitLSM using Put API...\n";

  mt19937 gen(42);
  uniform_int_distribution<int> cat_dist(0, 99);
  uniform_real_distribution<double> cont_dist(0.0, 100.0);
  uniform_int_distribution<size_t> char_dist(0, max_index);

  auto start_time = chrono::high_resolution_clock::now();

  for (uint64_t i = 1; i <= n; ++i) {
    vector<Attr> attrs(opts.attr_num);
    for (uint32_t j = 0; j < opts.attr_num; ++j) {
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
    cout << "✅ created " << n << " kvps. (total:"
         << chrono::duration_cast<chrono::milliseconds>(
                chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms elapsed)\n";
  }
}

static string format_double(double v) {
  ostringstream oss;
  oss << v;
  return oss.str();
}

static void save_csv(const string& output_dir, const string& exp_label,
                     uint64_t n, uint32_t payload_size, uint32_t attr_num,
                     double rho, const vector<ProgressLog>& progress_log) {
  filesystem::create_directories(output_dir);
  string filename = exp_label + "_bitlsm_n" + to_string(n) + "_p" +
                    to_string(payload_size) + "_a" + to_string(attr_num) +
                    "_rho" + format_double(rho) + ".csv";
  string full_path = output_dir + "/" + filename;
  ofstream f(full_path);
  f << "time_elapsed_ms,records_written\n";
  for (auto& log : progress_log)
    f << log.time_elapsed_ms << "," << log.records_written << "\n";
  f.close();
  cout << "Result saved to: " << full_path << "\n";
}

int main(const int argc, char* argv[]) {
  cxxopts::Options opts("bit-lsm", "BitLSM sequential write experiment");
  // clang-format off
  opts.add_options()
    ("h,help", "Print usage")
    ("exp_label", "Experiment label for output filename", cxxopts::value<string>()->default_value("seq_write"))
    ("n", "Total records to write", cxxopts::value<uint64_t>())
    ("p,payload_size", "Payload size in bytes", cxxopts::value<uint32_t>()->default_value("32"))
    ("a,attr_num", "Number of attributes per record", cxxopts::value<uint32_t>()->default_value("16"))
    ("rho", "BitLSM rho threshold", cxxopts::value<double>()->default_value("0.1"))
    ("d,db_path", "BitLSM storage path", cxxopts::value<string>())
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
  double rho = result["rho"].as<double>();
  string db_path = result["db_path"].as<string>();
  string output_dir = result["output_dir"].as<string>();

  BitLSMOptions bit_lsm_options;
  bit_lsm_options.rho = rho;
  bit_lsm_options.attr_num = attr_num;
  for (uint32_t i = 0; i < bit_lsm_options.attr_num; ++i) {
    if (i % 2 == 0)
      bit_lsm_options.attr_types.push_back(AttrType::CATEGORICAL);
    else
      bit_lsm_options.attr_types.push_back(AttrType::CONTINUOUS);
  }

  BitLSM db(db_path, bit_lsm_options);

  vector<ProgressLog> progress_log;
  fill_kvp_with_log(&db, n, bit_lsm_options, payload, progress_log, true);

  save_csv(output_dir, exp_label, n, payload, attr_num, rho, progress_log);

  return 0;
}
