#include "include/utils.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "sabi.h"
#include "sabi_iterator.h"
#include "table/block_based/block_based_table_reader.h"
#include <cstring>
#include <iostream>

using namespace std;
using namespace rocksdb;
using namespace roaring;
using namespace bitmap_index;

// CONSTANTS
const string db_path = "/scratch/data/user_defined_index_test";

// GLOBAL VAR
DB* db;
vector<ColumnFamilyHandle*> cf_handles;
Options options;
Status s;
SABIOptions sabi_option;

void test() {
  create_kvp(db, 1e7, 16, 32, 42, true);
  // FlushOptions flush_opts;
  // db->Flush(flush_opts);
  return;

  // 레벨별 SST 분포 확인 코드
  string stats;
  db->GetProperty("rocksdb.levelstats", &stats);
  cout << stats << "\n";

  // query samples
  SABIQuery query(
      {QueryCondition(1, CompareOp::GREATER_EQUAL, (double)24.00001),
       QueryCondition(1, CompareOp::LESS_EQUAL, (double)24.00002)});
  // SABIQuery query({QueryCondition(1, CompareOp::GREATER_EQUAL, (double)25.1),
  //  QueryCondition(1, CompareOp::LESS_EQUAL, (double)25.2)});
  // Point query sample
  // SABIQuery query({QueryCondition(1, CompareOp::EQUAL, (double)24.016234)});
  // Find all query sample
  // SABIQuery query({QueryCondition(1, CompareOp::GREATER_EQUAL, (double)0)});
  SABIIterator si(db, cf_handles[0], sabi_option, query);

  uint32_t total_cnt = 0;
  for (si.SeekToFirst(); si.Valid(); si.Next()) {
    cout << si.key().ToStringView() << ": ";
    si.TEST_DumpValue(si.value());
    total_cnt++;
  }
  cout << "total: " << total_cnt << "\n";
  return;
}

void configure_rocksdb_option() {
  options.create_if_missing = true;
  // 임시 SABI 옵션
  sabi_option.rho = 0.1;
  sabi_option.sk_num = 16;
  for (uint32_t i = 0; i < sabi_option.sk_num; ++i) {
    if (i % 2 == 0)
      sabi_option.sk_types.push_back(SKType::CATEGORICAL);
    else
      sabi_option.sk_types.push_back(SKType::CONTINUOUS);
  }

  BlockBasedTableOptions table_options;
  table_options.user_defined_index_factory =
      make_shared<bitmap_index::SABIFactory>(sabi_option);
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
}

int main(const int argc, char* argv[]) {
  // configure DB
  configure_rocksdb_option();
  const vector<ColumnFamilyDescriptor> column_families(
      {ColumnFamilyDescriptor(kDefaultColumnFamilyName, options)});
  s = DB::Open(options, db_path, column_families, &cf_handles, &db);
  if (!s.ok())
    cerr << "Failed to open DB: " << s.ToString() << "\n";
  assert(s.ok());

  test();

  // close DB gracefully
  for (auto handle : cf_handles) {
    db->DestroyColumnFamilyHandle(handle);
  }
  cf_handles.clear();
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db->WaitForCompact(wait_for_compact_options);
  assert(s.ok());
  delete db;
  cout << "DB successfully closed\n";

  return 0;
}
