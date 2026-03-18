#include "benchmark_experiment.h"
#include "bit_lsm.h"

using namespace std;
using namespace bit_lsm;

class BitLSMExperiment
    : public benchmark::BenchmarkExperiment<BitLSMExperiment> {
  unique_ptr<BitLSM> db_;
  double rho_ = 0.1;

 public:
  BitLSMExperiment() { method_name = "bitlsm"; }

  void Open(int argc, char* argv[], const string& db_path,
            const Schema& schema, bool) {
    cxxopts::Options opts("bit-lsm", "");
    opts.allow_unrecognised_options();
    // clang-format off
    opts.add_options()
      ("rho", "BitLSM rho threshold", cxxopts::value<double>()->default_value("0.1"));
    // clang-format on
    auto result = opts.parse(argc, argv);
    rho_ = result["rho"].as<double>();
    method_param_suffix = "_rho" + benchmark::format_double(rho_);

    BitLSMOptions options = schema.options;
    options.rho = rho_;
    db_ = make_unique<BitLSM>(db_path, options);
  }

  void Put(const string& pk, const vector<Attr>& attrs,
           const string& payload) {
    db_->Put(pk, attrs, payload);
  }

  benchmark::ReadResult Scan(BitLSMQuery& query, uint64_t n) {
    uint64_t matched = 0;
    auto start = chrono::high_resolution_clock::now();
    auto iter = db_->NewIterator(query);
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

  void Close() { db_.reset(); }
};

int main(const int argc, char* argv[]) {
  return BitLSMExperiment{}.Run(argc, argv);
}
