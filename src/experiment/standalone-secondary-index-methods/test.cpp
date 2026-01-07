#include "../workload_generator/workload_generator.cpp"
#include "composite_keys.h"
#include "eager_updates.h"
#include "lazy_updates.h"
#include "no_secondary_index.h"
#include "rocksdb/slice.h"
#include "standalone_secondary_index_experiment.h"
#include <chrono>
#include <cstdint>
#include <cxxopts.hpp>
#include <fstream>
#include <memory>

using namespace std;

string exp_label;

vector<IndexSpec> global_index_spec = {
    {"log_id", (uint64_t)1e8, DistType::AUTO_INCREMENT},
    {"request_uri", (uint64_t)1e5, DistType::SCRAMBLED_ZIPFIAN, 0.5},
    {"instance_id", (uint64_t)1e2, DistType::UNIFORM},
    {"region", (uint64_t)1e1, DistType::UNIFORM},
};

struct GetPerfLog {
  uint64_t time_elapsed_us;
  uint64_t response_time_us;
  uint32_t selectivity;
};

struct SetPerfLog {
  uint64_t time_elapsed_us;
  uint64_t response_time_us;
};

class StandaloneSITestDatabase : public IDatabase {
private:
  unique_ptr<StandaloneSecondaryIndexExperiment> db_impl;
  StandaloneSecondaryIndexExperiment::CompositeQueryRunStrategy
      composition_strategy;
  uint64_t qid = 0;
  vector<GetPerfLog> get_perf_log;
  vector<SetPerfLog> set_perf_log;
  string output_file_path;
  chrono::time_point<chrono::system_clock,
                     chrono::duration<long, ratio<1, 1000000000>>>
      db_start_time;

public:
  // Mock Monitoring Server Log DB
  // PK제외 선택도 내림차순으로 정렬필요
  // (post filtering시 제1 보조 인덱스를 기준 인덱스로 사용하기 때문)
  vector<IndexSpec> specs = global_index_spec;

  StandaloneSITestDatabase(
      unique_ptr<StandaloneSecondaryIndexExperiment> db_impl,
      StandaloneSecondaryIndexExperiment::CompositeQueryRunStrategy
          composition_strategy,
      string output_file_path)
      : db_impl(std::move(db_impl)), composition_strategy(composition_strategy),
        output_file_path(output_file_path) {
    get_perf_log.reserve(100000);

    db_start_time = chrono::high_resolution_clock::now();
  }

  ~StandaloneSITestDatabase() {
    SaveGetLog();
    SaveSetLog();
  }

  void SaveGetLog() {
    cout << "Saving Get Log (" << get_perf_log.size() << ")...\n";
    if (get_perf_log.size() == 0)
      return;
    string full_path = output_file_path + "/" + exp_label + ".csv";
    ofstream log_file(full_path);

    log_file << "time_elapsed_us,response_time_us,selectivity\n";
    for (auto l : get_perf_log)
      log_file << l.time_elapsed_us - get_perf_log[0].time_elapsed_us << ","
               << l.response_time_us << "," << l.selectivity << "\n";

    log_file.close();
  }

  void SaveSetLog() {
    cout << "Saving Set Log (" << set_perf_log.size() << ")...\n";
    if (set_perf_log.size() == 0)
      return;
    string full_path = output_file_path + "/" + exp_label + ".csv";
    ofstream log_file(full_path);

    log_file << "time_elapsed_us,response_time_us\n";
    for (auto l : set_perf_log)
      log_file << l.time_elapsed_us << "," << l.response_time_us << "\n";

    log_file.close();
  }

  void Set(const string& key, const string& val) override {
    auto start = std::chrono::high_resolution_clock::now();
    db_impl->Insert(key, val);
    auto now = std::chrono::high_resolution_clock::now();
    auto dur =
        std::chrono::duration_cast<std::chrono::microseconds>(now - start)
            .count();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                       now - db_start_time)
                       .count();
    set_perf_log.push_back({
        static_cast<uint64_t>(elapsed),
        static_cast<uint64_t>(dur),
    });
  }
  void Get(const vector<pair<uint32_t, rocksdb::Slice>>& query,
           vector<pair<string, string>>& results) override {
    qid++;
    auto start = std::chrono::high_resolution_clock::now();
    db_impl->GetBySecondaryIndices(query, composition_strategy, &results);
    auto now = std::chrono::high_resolution_clock::now();
    auto dur =
        std::chrono::duration_cast<std::chrono::microseconds>(now - start)
            .count();
    uint32_t selectivity = results.size();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                       now - db_start_time)
                       .count();

    get_perf_log.push_back({static_cast<uint64_t>(elapsed),
                            static_cast<uint64_t>(dur), selectivity});
  }
};

int main(const int argc, char* argv[]) {
  // Test option definition
  cxxopts::Options options("SIExperiment",
                           "Test for LSM-Tree SI for composite index queries");
  // clang-format off
  options.add_options()
    ("h,help", "Print usage")
    // required
    ("b,baseline", "Target baseline", cxxopts::value<string>())
    ("n", "Total row N", cxxopts::value<uint32_t>())
    ("r,read_ratio", "Read ratio", cxxopts::value<double>())
    ("d,db_path", "DB file path", cxxopts::value<string>())
    ("o,output_file_path", "Output file path", cxxopts::value<string>())
    ("l,label", "Label of experiement", cxxopts::value<string>())
    // optional
    ("p,payload_size", "(Optional) Size of payload in bytes", cxxopts::value<uint32_t>()->default_value("4096"));
  // clang-format on

  cxxopts::ParseResult result = options.parse(argc, argv);
  if (result.count("help")) {
    cout << options.help() << "\n";
    exit(0);
  }
  string baseline = result["baseline"].as<string>();
  uint32_t n = result["n"].as<uint32_t>();
  double read_ratio = result["read_ratio"].as<double>();
  string output_file_path = result["output_file_path"].as<string>();
  exp_label = result["label"].as<string>();
  string db_path = result["db_path"].as<string>();
  uint32_t payload_size = result["payload_size"].as<uint32_t>();

  string composition_arg = baseline.substr(0, 2);
  string secondary_index_arg = baseline.substr(3, 2);

  unique_ptr<StandaloneSecondaryIndexExperiment> db_impl;
  StandaloneSecondaryIndexExperiment::CompositeQueryRunStrategy
      composition_strategy;

  if (composition_arg == "NO") {
    composition_strategy = StandaloneSecondaryIndexExperiment::
        CompositeQueryRunStrategy::kFullTableScan;
  } else if (composition_arg == "PF") {
    composition_strategy = StandaloneSecondaryIndexExperiment::
        CompositeQueryRunStrategy::kPostFiltering;
  } else if (composition_arg == "IM") {
    composition_strategy = StandaloneSecondaryIndexExperiment::
        CompositeQueryRunStrategy::kIndexMerge;
  } else {
    cerr << "composition_arg " << composition_arg << " is not supported.\n";
  }

  uint32_t si_cnt = global_index_spec.size() - 1; // except pk
  if (composition_arg == "NO") { // if starts with "NO", drop si_arg
    db_impl = StandaloneSecondaryIndexExperiment::Create<NoSecondaryIndex>(
        db_path, si_cnt);
  } else if (secondary_index_arg == "EU") {
    db_impl = StandaloneSecondaryIndexExperiment::Create<EagerUpdates>(db_path,
                                                                       si_cnt);
  } else if (secondary_index_arg == "LU") {
    db_impl = StandaloneSecondaryIndexExperiment::Create<LazyUpdates>(db_path,
                                                                      si_cnt);
  } else if (secondary_index_arg == "CK") {
    db_impl = StandaloneSecondaryIndexExperiment::Create<CompositeKeys>(db_path,
                                                                        si_cnt);
  } else {
    cerr << "secondary_index_arg " << secondary_index_arg
         << " is not supported.\n";
  }

  StandaloneSITestDatabase db(std::move(db_impl), composition_strategy,
                              output_file_path);

  WorkloadGenerator wg(db, db.specs, payload_size);
  wg.Generate(n, read_ratio, {0, 0, 1, 0}, {0, 1, 1, 1});

  return 0;
}
