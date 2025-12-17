#include <chrono>
#include <iostream>
#include <random>

#include "include/sst_attached_bitmap_index_builder.h"
#include "include/utils.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "table/block_based/block_builder.h"

using namespace std;
using namespace rocksdb;

// CONSTANTS
const string db_path = "/scratch/data/block_prefix_sum_test";
const string server_address = "0.0.0.0:50051";

// GLOBAL VAR
DB* db;
Options options;
Status status;
chrono::_V2::system_clock::time_point start_time, end_time;
chrono::milliseconds ms_duration;

void configure_rocksdb_option() {
  options.create_if_missing = true;
  // TODO: CF별 다른 bitset 생성 규칙 생성할 수 있도록 분리 필요
  // 현재는 기본 CF에 대해서 실험중
  options.table_properties_collector_factories.push_back(
      make_shared<SABIBuilderFactory>());
}

void test() {
  // create_kvp(db, 1e8);
  // inspect_sst("/scratch/data/block_prefix_sum_test/000290.sst");

  // WIP - RocksDB internal API 연결 테스트 필요
}

int main(const int argc, char* argv[]) {
  // configure DB
  configure_rocksdb_option();
  status = DB::Open(options, db_path, &db);
  assert(status.ok());

  ////////////////////////////////////////////////////////////////////////////////////////////////////
  test();
  ////////////////////////////////////////////////////////////////////////////////////////////////////

  // close DB gracefully
  WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
  wait_for_compact_options.close_db = true;
  status = db->WaitForCompact(wait_for_compact_options);
  assert(status.ok());
  delete db;
  cout << "DB successfully closed\n";

  return 0;
}
