#include <cstring>
#include <iostream>

#include "include/utils.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "sabi.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;
using namespace bitmap_index;

// CONSTANTS
const string db_path = "/scratch/data/user_defined_index_test";

// GLOBAL VAR
DB* db;
Options options;
Status s;
SABIOptions sabi_option;

void test() {
  // scratch //
  create_kvp(db, 1e7, 16);
  return;

  // read test (수정필요)
  string result;
  ReadOptions sabi_ro;
  sabi_ro.table_index_factory = new bitmap_index::SABIFactory(sabi_option);

  rocksdb::Iterator* it = db->NewIterator(sabi_ro);
  const rocksdb::Comparator* bytewise_cmp = BytewiseComparator();
  MultiScanArgs scan_opts = MultiScanArgs(bytewise_cmp);
  scan_opts.use_async_io = true; // scan option 전파를 위해 필요한 옵션

  // unordered_map<string, string> property_bag = {{"qc", "3"}};
  // string start_key = "789346";
  // 임의 upper bound (TODO: 없어도 되는지 확인 필요)
  // string end_key = "99999999999999999999";

  // scan_opts.insert(start_key, end_key, property_bag);
  // it->Prepare(scan_opts);

  // it->Seek(start_key); // WIP - SeekToFirst not supported 문제 확인중
  // cout << "status: " << it->status().ToString() << "\n";

  // assert(it->Valid());
  // cout << "found!: " << it->value().ToString() << "\n";
  // for (it->SeekToFirst(); it->Valid(); it->Next()) {
  //   // Do something with it->key() and it->value().
  // cout << it->key().ToString() << " " << it->value().ToString() << "\n";
  // }
  // delete it;
}

void configure_rocksdb_option() {
  options.create_if_missing = true;

  BlockBasedTableOptions table_options;
  table_options.user_defined_index_factory =
      make_shared<bitmap_index::SABIFactory>(sabi_option);
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));
}

int main(const int argc, char* argv[]) {
  // configure DB
  configure_rocksdb_option();
  s = DB::Open(options, db_path, &db);
  assert(s.ok());

  ////////////////////////////////////////////////////////////////////////////////////////////////////
  test();
  ////////////////////////////////////////////////////////////////////////////////////////////////////

  // close DB gracefully
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  s = db->WaitForCompact(wait_for_compact_options);
  assert(s.ok());
  delete db;
  cout << "DB successfully closed\n";

  return 0;
}
