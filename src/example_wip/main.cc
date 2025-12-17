#include <iostream>

#include "db/db_impl/db_impl.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "include/block_based_table_sabi_filter.h"
#include "include/sst_attached_bitmap_index_builder.h"
#include "include/utils.h"
#include "options/cf_options.h"
#include "rocksdb/db.h"
#include "rocksdb/file_system.h"
#include "rocksdb/options.h"
#include "table/block_based/block_based_table_builder.h"
#include "table/block_based/block_based_table_reader.h"

using namespace std;
using namespace rocksdb;
using namespace sabi;

// CONSTANTS
const string db_path = "/scratch/data/block_prefix_sum_test";
const string server_address = "0.0.0.0:50051";

// GLOBAL VAR
DB* db;
Options options;
Status s;

void test() {
  DBImpl* db_impl = static_cast<DBImpl*>(db);
  VersionSet* vs = db_impl->GetVersionSet();
  ColumnFamilySet* cf_set = vs->GetColumnFamilySet();
  ColumnFamilyData* cfd = cf_set->GetColumnFamily("default");
  Version* v = cfd->current();
  TableCache* tc = cfd->table_cache();
  TableCache::CacheInterface cache_interface = tc->get_cache();

  // v->Ref(); // TODO: 동시성 제어를 위해 ref count 올리는 부분 나중에 실험
  // 코드에 추가 필요 (db_impl->mutex()->Lock() 등 포함)

  // FindTable 호출을 위한 파라미터들
  const ReadOptions& read_options = ReadOptions();
  const FileOptions& file_options = FileOptions();
  VersionStorageInfo* storage_info = v->storage_info();
  const InternalKeyComparator* icmp = storage_info->InternalComparator();
  const MutableCFOptions& cf_opts = cfd->GetLatestMutableCFOptions();
  FileMetaData* file_meta = nullptr;
  const bool no_io = false;

  // test: level 6의 첫번째 SST 선택 (예시)
  const vector<FileMetaData*>& files = storage_info->LevelFiles(6);
  if (!files.empty()) {
    file_meta = files[0];
  } else {
    cout << "leve 7 SST non exist\n";
  }

  TableCache::TypedHandle* table_handle = nullptr;
  s = tc->FindTable(read_options, file_options, *icmp, *file_meta,
                    &table_handle, cf_opts,
                    no_io); // TODO: 뒤에 optional parameter도 의미 파악 필요

  if (!s.ok()) {
    cout << "Debug: fail to find TableReader\n";
  }
  TableReader* table = cache_interface.Value(table_handle);
  BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);
  if (bbt == nullptr) {
    cout << "BBT not found\n";
  }
  BlockBasedTableSABIFilter sabi_filter(bbt);
  sabi_filter.test();
}

void configure_rocksdb_option() {
  options.create_if_missing = true;
  // TODO: CF별 다른 bitset 생성 규칙 생성할 수 있도록 분리 필요
  // 현재는 기본 CF에 대해서 실험중
  options.table_properties_collector_factories.push_back(
      make_shared<SABIBuilderFactory>());
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
