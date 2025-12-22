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

// CONSTANTS
const string db_path = "/scratch/data/user_defined_index_test";
const string server_address = "0.0.0.0:50051";

// GLOBAL VAR
DB* db;
Options options;
Status s;

void test() {
  // scratch //
  // create_kvp(db, 1e8, 16);
  string result;
  ReadOptions sabi_ro;
  sabi_ro.table_index_factory = new bitmap_index::SABIFactory();

  db->Get(sabi_ro, "5234", &result);
  cout << result << "\n";

  rocksdb::Iterator* it = db->NewIterator(sabi_ro);
  for (it->Seek("123"); it->Valid(); it->Next()) {
    // Do something with it->key() and it->value().
    cout << it->key().ToString() << " " << it->value().ToString() << "\n";
  }
}

void configure_rocksdb_option() {
  options.create_if_missing = true;

  BlockBasedTableOptions table_options;
  table_options.user_defined_index_factory =
      make_shared<bitmap_index::SABIFactory>();
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
