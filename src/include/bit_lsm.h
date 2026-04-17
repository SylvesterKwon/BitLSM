#pragma once

#include "bit_lsm_iterator.h"
#include "bit_lsm_query.h"
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <string>

using Attr = std::variant<double, std::string>;

namespace bit_lsm {

class BitLSM {
private:
  rocksdb::DB* db_;
  std::string db_path_;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  rocksdb::Options rocksdb_options_;
  BitLSMOptions bit_lsm_options_;

  // Helper functions

public:
  BitLSM(const std::string& db_path, const BitLSMOptions& bit_lsm_options,
         const rocksdb::Options& rocksdb_options,
         const rocksdb::BlockBasedTableOptions& table_options);
  ~BitLSM();
  // BitLSM core API
  rocksdb::Status Put(const std::string& pk, const std::vector<Attr>& attrs,
                      const std::string& payload);
  rocksdb::Status PutBatch(const std::vector<std::string>& pks,
                           const std::vector<std::vector<Attr>>& attrs_list,
                           const std::vector<std::string>& payloads);
  rocksdb::Status Delete(const std::string& key);
  std::unique_ptr<BitLSMIterator> NewIterator(BitLSMQuery& query);

  // For debug
  void Statistics();
  // ...
};
} // namespace bit_lsm