#include "bit_lsm.h"
#include "bit_lsm_utils.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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

static void seq_put_worker(bit_lsm::BitLSM* db, uint32_t thread_id, uint64_t n,
                           uint32_t payload_size, bit_lsm::BitLSMOptions opts,
                           atomic<uint64_t>& global_auto_increment,
                           chrono::_V2::system_clock::time_point start_time,
                           vector<ProgressLog>& progress_log,
                           mutex& log_mutex) {
  mt19937 gen(thread_id);
  uniform_int_distribution<int> cat_dist(0, 99);
  uniform_real_distribution<double> cont_dist(0.0, 100.0);
  uniform_int_distribution<size_t> char_dist(0, max_index);

  while (true) {
    uint64_t current_pk_val = ++global_auto_increment;
    if (current_pk_val > n)
      break;

    vector<Attr> attrs(opts.attr_num);
    for (uint32_t j = 0; j < opts.attr_num; ++j) {
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

    db->Put(pk, attrs, payload);

    if (current_pk_val % 1000000 == 0) {
      auto elapsed_ms = chrono::duration_cast<chrono::milliseconds>(
                            chrono::high_resolution_clock::now() - start_time)
                            .count();
      {
        lock_guard<mutex> lock(log_mutex);
        progress_log.push_back(
            {static_cast<uint64_t>(elapsed_ms), current_pk_val});
      }
      cout << "putted: " << current_pk_val << " kvps, elapsed: " << elapsed_ms
           << "ms\n";
    }
  }
}

static void fill_kvp_with_log(bit_lsm::BitLSM* db, uint32_t num_threads,
                              uint64_t n, bit_lsm::BitLSMOptions opts,
                              uint32_t payload_size,
                              vector<ProgressLog>& progress_log,
                              bool debug = false) {
  if (debug)
    cout << "creating " << n << " kvps into BitLSM using Put API...\n";

  auto start_time = chrono::high_resolution_clock::now();
  atomic<uint64_t> global_auto_increment(0);
  mutex log_mutex;

  vector<thread> workers;
  for (int i = 0; i < num_threads; ++i)
    workers.emplace_back(seq_put_worker, db, i, n, payload_size, opts,
                         std::ref(global_auto_increment), start_time,
                         std::ref(progress_log), std::ref(log_mutex));
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

static string format_double(double v) {
  ostringstream oss;
  oss << v;
  return oss.str();
}

static void save_csv(const string& output_dir, const string& exp_type,
                     uint64_t n, uint32_t payload_size, uint32_t num_threads,
                     uint32_t attr_num, double rho,
                     const vector<ProgressLog>& progress_log) {
  filesystem::create_directories(output_dir);
  string filename = exp_type + "_bitlsm_n" + to_string(n) + "_p" +
                    to_string(payload_size) + "_t" + to_string(num_threads) +
                    "_a" + to_string(attr_num) + "_rho" + format_double(rho) +
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
  cxxopts::Options opts("bit-lsm", "BitLSM sequential write experiment");
  // clang-format off
  opts.add_options()
    ("h,help", "Print usage")
    ("exp_type", "Experiment type", cxxopts::value<string>()->default_value("seq_write"))
    ("n", "Total records to write", cxxopts::value<uint64_t>())
    ("t,threads", "Number of writer threads", cxxopts::value<uint32_t>()->default_value("4"))
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

  string exp_type = result["exp_type"].as<string>();
  uint64_t n = result["n"].as<uint64_t>();
  uint32_t n_threads = result["threads"].as<uint32_t>();
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
  fill_kvp_with_log(&db, n_threads, n, bit_lsm_options, payload, progress_log,
                    true);

  save_csv(output_dir, exp_type, n, payload, n_threads, attr_num, rho,
           progress_log);

  return 0;
}
