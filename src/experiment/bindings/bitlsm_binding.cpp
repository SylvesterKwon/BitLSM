#include "bitlsm_binding.h"
#include "benchmark_experiment.h"
#include <chrono>
#include <cxxopts.hpp>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

namespace experiment {

void BitLSMBinding::Open(int argc, char* argv[], const std::string& db_path,
                          const BitLSMOptions& opts) {
  cxxopts::Options cxx("bitlsm", "");
  cxx.allow_unrecognised_options();
  cxx.add_options()("rho", "BitLSM rho threshold",
                    cxxopts::value<double>()->default_value("0.1"));
  auto result = cxx.parse(argc, argv);
  rho_ = result["rho"].as<double>();

  rocksdb::Options rocksdb_options;
  rocksdb_options.create_if_missing = true;
  rocksdb_options.max_background_jobs = 6;
  rocksdb_options.bytes_per_sync = 1048576;
  rocksdb_options.compaction_pri = rocksdb::kMinOverlappingRatio;
  rocksdb_options.max_write_buffer_number = 5;
  rocksdb::BlockBasedTableOptions table_options;
  table_options.block_size = 4 * 1024;

  BitLSMOptions bitlsm_opts = opts;
  bitlsm_opts.rho = rho_;
  db_ = std::make_unique<bit_lsm::BitLSM>(db_path, bitlsm_opts,
                                            rocksdb_options, table_options);
}

void BitLSMBinding::Put(const std::string& pk, const std::vector<Attr>& attrs,
                         const std::string& payload) {
  db_->Put(pk, attrs, payload);
}

ScanResult BitLSMBinding::Scan(BitLSMQuery& query) {
  uint64_t matched = 0;
  auto start = std::chrono::high_resolution_clock::now();
  auto iter = db_->NewIterator(query);
  for (iter->SeekToFirst(); iter->Valid(); iter->Next())
    matched++;
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::high_resolution_clock::now() - start)
                     .count();
  return {static_cast<uint64_t>(elapsed), matched};
}

void BitLSMBinding::Close() { db_.reset(); }

std::string BitLSMBinding::ParamSuffix() const {
  return "_rho" + benchmark::format_double(rho_);
}

}  // namespace experiment
