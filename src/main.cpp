#include "bit_lsm.h"
#include "bit_lsm_query.h"
#include "include/utils.h"
#include "sabi.h"
#include <cstring>

using namespace std;
using namespace rocksdb;
using namespace roaring;
using namespace bit_lsm;

void test() {
  // create_kvp(db, 1e7, 16, 32, 42, true);
  // FlushOptions flush_opts;
  // db->Flush(flush_opts);
  // return;

  // 레벨별 SST 분포 확인 코드
  // string stats;
  // db->GetProperty("rocksdb.levelstats", &stats);
  // cout << stats << "\n";

  // query samples
  BitLSMQuery query(
      {QueryCondition(1, CompareOp::GREATER_EQUAL, (double)24.0001),
       QueryCondition(1, CompareOp::LESS_EQUAL, (double)24.0002)});
  // BitLSMQuery query({QueryCondition(1, CompareOp::GREATER_EQUAL,
  // (double)25.1),
  //  QueryCondition(1, CompareOp::LESS_EQUAL, (double)25.2)});
  // Point query sample
  // BitLSMQuery query({QueryCondition(1, CompareOp::EQUAL,
  // (double)24.016234)}); Find all query sample BitLSMQuery
  // query({QueryCondition(1, CompareOp::GREATER_EQUAL, (double)0)});
}

int main(const int argc, char* argv[]) {
  string db_path = "/scratch/data/user_defined_index_test";
  BitLSMOptions sabi_options;
  sabi_options.rho = 0.1;
  sabi_options.sk_num = 16;
  for (uint32_t i = 0; i < sabi_options.sk_num; ++i) {
    if (i % 2 == 0)
      sabi_options.sk_types.push_back(SKType::CATEGORICAL);
    else
      sabi_options.sk_types.push_back(SKType::CONTINUOUS);
  }
  BitLSM db(db_path, sabi_options);
  test();

  return 0;
}
