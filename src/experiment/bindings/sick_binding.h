#pragma once
#include "binding.h"
#include "si_benchmark_common.h"
#include <string>

namespace experiment {

class SICKBinding : public Binding {
  benchmark::SIDBHandles db_;
  BitLSMOptions options_;
  benchmark::SIStrategy strategy_ = benchmark::SIStrategy::kIndexMerge;
  rocksdb::WriteOptions wo_;
  std::string serialized_value_;
  static constexpr uint32_t idx_no_prefix_size_ = 4;
  static constexpr uint32_t si_prefix_length_ = 16;
  std::string si_key_buf_;

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  std::string Name() const override { return "si-ck"; }
  std::string ParamSuffix() const override;
};

}  // namespace experiment
