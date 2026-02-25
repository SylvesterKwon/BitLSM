#include <cstring>
#include <iostream>

#include "db/db_impl/db_impl.h"
#include "include/utils.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "sabi.h"
#include "sabi_iterator.h"
#include "table/block_based/block_based_table_reader.h"

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
  // create_kvp(db, 1e7, 16);
  // return;
  // FlushOptions flush_opts;
  // db->Flush(flush_opts);
  // return;

  // read test
  DBImpl* db_impl = static_cast<DBImpl*>(db);
  VersionSet* vs = db_impl->GetVersionSet();
  ColumnFamilySet* cf_set = vs->GetColumnFamilySet();
  ColumnFamilyData* cfd = cf_set->GetColumnFamily("default");
  Version* v = cfd->current();
  TableCache* tc = cfd->table_cache();
  TableCache::CacheInterface cache_interface = tc->get_cache();

  // FindTable 호출을 위한 파라미터들
  const ReadOptions& read_options = ReadOptions();
  const FileOptions& file_options = FileOptions();
  VersionStorageInfo* storage_info = v->storage_info();
  const InternalKeyComparator* icmp = storage_info->InternalComparator();
  const MutableCFOptions& cf_opts = cfd->GetLatestMutableCFOptions();
  FileMetaData* file_meta = nullptr;
  const bool no_io = false;

  // 레벨별 SST 분포 확인 코드
  // string stats;
  // db->GetProperty("rocksdb.levelstats", &stats);
  // cout << stats << "\n";

  const vector<FileMetaData*>& files = storage_info->LevelFiles(6);
  assert(!files.empty());
  file_meta = files[0];
  TableCache::TypedHandle* table_handle = nullptr;
  s = tc->FindTable(read_options, file_options, *icmp, *file_meta,
                    &table_handle, cf_opts,
                    no_io); // TODO: 뒤에 optional parameter도 의미 파악 필요
  if (!s.ok())
    cout << "Debug: fail to find TableReader\n";

  TableReader* table = cache_interface.Value(table_handle);
  BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);
  if (bbt == nullptr)
    cout << "BBT not found\n";

  SABIQuery query({QueryCondition(0, CompareOp::EQUAL, "1")});
  SABITableIterator sti(sabi_option, bbt, query);
  sti.test();
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
