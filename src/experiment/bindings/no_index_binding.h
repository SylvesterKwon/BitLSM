#pragma once
#include "binding.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <string>
#include <vector>

namespace experiment {

class NoIndexBinding : public Binding {
  rocksdb::DB* db_ = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  BitLSMOptions options_;
  rocksdb::WriteOptions wo_;
  std::string serialized_value_;

 public:
  void Open(int argc, char* argv[], const std::string& db_path,
            const BitLSMOptions& opts) override;
  void Put(const std::string& pk, const std::vector<Attr>& attrs,
           const std::string& payload) override;
  ScanResult Scan(BitLSMQuery& query) override;
  void Close() override;
  std::string Name() const override { return "no-index"; }
};

}  // namespace experiment
