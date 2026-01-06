#include "../workload_generator/workload_generator.cpp"
#include "composite_keys.h"
#include "eager_updates.h"
#include "lazy_updates.h"
#include "no_secondary_index.h"
#include "standalone_secondary_index_experiment.h"
#include <cstdint>
#include <cxxopts.hpp>
#include <fstream>
#include <memory>

using namespace std;

class StandaloneSITestDatabase : public IDatabase {
private:
  unique_ptr<StandaloneSecondaryIndexExperiment> db_impl;
  StandaloneSecondaryIndexExperiment::CompositeQueryRunStrategy
      composition_strategy;
  uint64_t qid = 0;
  vector<uint64_t> latencies;
  string output_file_path;
  string experiment_label;

public:
  // Mock Monitoring Server Log DB
  // PK제외 선택도 내림차순으로 정렬필요
  // (post filtering시 제1 보조 인덱스를 기준 인덱스로 사용하기 때문)
  vector<IndexSpec> specs = {
      {"log_id", (uint64_t)1e7, DistType::AUTO_INCREMENT},
      // {"request_uri", (uint64_t)1e5, DistType::SCRAMBLED_ZIPFIAN, 0.99},
      {"request_uri", (uint64_t)1e5, DistType::UNIFORM}};
  // {"instance_id", (uint64_t)1e2, DistType::UNIFORM},
  // {"region", (uint64_t)1e1, DistType::UNIFORM}};

  StandaloneSITestDatabase(
      unique_ptr<StandaloneSecondaryIndexExperiment> db_impl,
      StandaloneSecondaryIndexExperiment::CompositeQueryRunStrategy
          composition_strategy,
      string output_file_path, string experiment_label)
      : db_impl(std::move(db_impl)), composition_strategy(composition_strategy),
        output_file_path(output_file_path), experiment_label(experiment_label) {
    latencies.reserve(1000000);
  }

  ~StandaloneSITestDatabase() { SaveLatencyLog(); }

  void SaveLatencyLog() {
    string latency_log_full_path = output_file_path + "/latency-log.csv";
    ifstream check_file(latency_log_full_path);
    bool is_empty =
        check_file.peek() ==
        ifstream::traits_type::eof(); // 파일이 없거나 비어있으면 true
    check_file.close();

    ofstream latency_log(latency_log_full_path, std::ios::app);

    // Write header
    if (is_empty) {
      latency_log << "experiment_label,latency_us\n";
    }

    for (auto l : latencies) {
      latency_log << experiment_label << "," << l << "\n";
    }

    latency_log.close(); // 명시적 닫기
    ///

    std::sort(latencies.begin(), latencies.end());
    double sum = 0;
    for (auto l : latencies)
      sum += l;
    cout << "=== Latency Statistics (us) ===\n";
    cout << "Avg: " << sum / latencies.size() << "\n";
    cout << "Min: " << latencies.front() << "\n";
    cout << "Max: " << latencies.back() << "\n";
    cout << "P50 (Median): " << latencies[latencies.size() * 0.50] << "\n";
    cout << "P99: " << latencies[latencies.size() * 0.99] << "\n";
    cout << "P99.9: " << latencies[latencies.size() * 0.999] << "\n";
    // set_log.close(), get_log.close();
  }

  void Set(const string& key, const string& val) override {
    // output_file << key << "," << val << "\n";
    auto start = std::chrono::high_resolution_clock::now();
    db_impl->Insert(key, val);
    auto end = std::chrono::high_resolution_clock::now();
    auto dur =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();
    latencies.push_back(dur);
  }
  void Get(const vector<pair<uint32_t, rocksdb::Slice>>& query,
           vector<pair<string, string>>& results) override {
    qid++;
    auto start = std::chrono::high_resolution_clock::now();
    db_impl->GetBySecondaryIndices(query, composition_strategy, &results);
    auto end = std::chrono::high_resolution_clock::now();
    auto dur =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();
    latencies.push_back(dur);
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

  if (composition_arg == "NO") { // if starts with "NO", drop si_arg
    db_impl = StandaloneSecondaryIndexExperiment::Create<NoSecondaryIndex>(
        db_path, 3);
  } else if (secondary_index_arg == "EU") {
    db_impl =
        StandaloneSecondaryIndexExperiment::Create<EagerUpdates>(db_path, 3);
  } else if (secondary_index_arg == "LU") {
    db_impl =
        StandaloneSecondaryIndexExperiment::Create<LazyUpdates>(db_path, 3);
  } else if (secondary_index_arg == "CK") {
    db_impl =
        StandaloneSecondaryIndexExperiment::Create<CompositeKeys>(db_path, 3);
  } else {
    cerr << "secondary_index_arg " << secondary_index_arg
         << " is not supported.\n";
  }

  StandaloneSITestDatabase db(std::move(db_impl), composition_strategy,
                              output_file_path, baseline);

  WorkloadGenerator wg(db, db.specs, payload_size);
  // wg.Generate(n, read_ratio, {0, 0, 2, 1}, {0, 1, 1, 1});
  wg.Generate(n, read_ratio, {0, 1}, {0, 1});

  return 0;
}
