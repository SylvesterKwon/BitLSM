#pragma once
#include "binding.h"
#include "bit_lsm.h"
#include <memory>

namespace experiment {

class BitLSMBinding : public Binding {
  std::unique_ptr<bit_lsm::BitLSM> db_;
  double rho_ = 0.1;

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  std::string Name() const override { return "bitlsm"; }
  std::string ParamSuffix() const override;
};

}  // namespace experiment
