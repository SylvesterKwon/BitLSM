#include <cstring>
#include <iostream>

#include "db/column_family.h"
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
  // create_kvp(db, 1e7, 16);
  // FlushOptions flush_opts;
  // db->Flush(flush_opts);
  // return;

  // read test
  DBImpl* db_impl = static_cast<DBImpl*>(db);
  VersionSet* vs = db_impl->GetVersionSet();
  ColumnFamilySet* cf_set = vs->GetColumnFamilySet();
  ColumnFamilyData* cfd = cf_set->GetColumnFamily("default");

  // 레벨별 SST 분포 확인 코드
  string stats;
  db->GetProperty("rocksdb.levelstats", &stats);
  cout << stats << "\n";

  // query samples
  // SABIQuery query({QueryCondition(0, CompareOp::EQUAL, "25"),
  //                  QueryCondition(2, CompareOp::EQUAL, "24")});
  // SABIQuery query({QueryCondition(1, CompareOp::GREATER_EQUAL, (double)25.1),
  //  QueryCondition(1, CompareOp::LESS_EQUAL, (double)25.2)});
  // L0 point query sample
  SABIQuery query({QueryCondition(1, CompareOp::EQUAL, (double)43.877001)});
  // L5 point query sample
  // SABIQuery query({QueryCondition(1, CompareOp::EQUAL, (double)77.597704)});
  // L6 point query sample
  // SABIQuery query({QueryCondition(1, CompareOp::EQUAL, (double)25.161031)});
  // Find all query sample
  // SABIQuery query({QueryCondition(1, CompareOp::GREATER_EQUAL, (double)0)});

  // SABILevelIterator sli(cfd, 6, sabi_option, query);
  // uint32_t total_cnt = 0;
  // for (sli.SeekToFirst(); sli.Valid(); sli.Next()) {
  //   cout << sli.key().ToStringView() << ": ";
  //   sli.TEST_DumpValue(sli.value());
  //   total_cnt++;
  // }
  // cout << "total: " << total_cnt << "\n";

  // TODO: Query 전달하는 최상위 class 작업할시 seqno 확정해줘야함
  // 지금은 임시로 생성
  // 이하 getreferencedsuperversion 도 wrapper에 포함되어야함
  sabi_option.read_seqno = db_impl->GetLatestSequenceNumber();
  SuperVersion* sv(cfd->GetReferencedSuperVersion(db_impl));
  SABIMergingIterator smi(sv, sabi_option, query);
  uint32_t total_cnt = 0;
  for (smi.SeekToFirst(); smi.Valid(); smi.Next()) {
    cout << smi.key().ToStringView() << ": ";
    smi.TEST_DumpValue(smi.value());
    total_cnt++;
  }
  cout << "total: " << total_cnt << "\n";
  // TODO: 이터레이터도 delete해야하는거 아닌가? 왜 delete가 안된다고하지?

  // TODO: sv unref에 대한 책임도 바깥 레이어에서 가져감. 아래와 같이 수행할것
  if (sv->Unref()) {
    db_impl->mutex()->Lock();
    sv->Cleanup();
    db_impl->mutex()->Unlock();
    delete sv;
  }

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
