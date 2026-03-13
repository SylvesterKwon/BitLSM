#include "bit_lsm.h"
#include "bit_lsm_utils.h"
#include "sabi.h"
#include "utils.h"
#include <cstdint>
#include <cstring>

using namespace std;
using namespace rocksdb;
using namespace roaring;
using namespace bit_lsm;

string db_path = "/scratch/data/test-1e8-32-0.1-t4";
uint32_t num_threads = 4;
int main(const int argc, char* argv[]) {
  // test {# of kvp, payload bytes, rho}

  // 테스트용 옵션
  BitLSMOptions bit_lsm_options;
  bit_lsm_options.rho = 0.1;
  bit_lsm_options.attr_num = 16;
  for (uint32_t i = 0; i < bit_lsm_options.attr_num; ++i) {
    if (i % 2 == 0)
      bit_lsm_options.attr_types.push_back(AttrType::CATEGORICAL);
    else
      bit_lsm_options.attr_types.push_back(AttrType::CONTINUOUS);
  }

  BitLSM db(db_path, bit_lsm_options);

  // Write test
  fill_kvp_multi_thread(&db, num_threads, 1e8, bit_lsm_options, 32, 42, true);
  return 0;

  chrono::_V2::system_clock::time_point start_time =
      chrono::high_resolution_clock::now();

  // BitLSMQuery query({
  //     QueryCondition(1, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(1, CompareOp::LESS, (double)1),
  //     QueryCondition(3, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(3, CompareOp::LESS, (double)1),
  //     QueryCondition(5, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(5, CompareOp::LESS, (double)1),
  //     QueryCondition(7, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(7, CompareOp::LESS, (double)1),
  // });

  BitLSMQuery query({
      QueryCondition(1, CompareOp::GREATER_EQUAL, (double)0),
      QueryCondition(1, CompareOp::LESS, (double)10),
      // QueryCondition(3, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(3, CompareOp::LESS, (double)10),
      // QueryCondition(5, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(5, CompareOp::LESS, (double)10),
      // QueryCondition(7, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(7, CompareOp::LESS, (double)10),
      // QueryCondition(9, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(9, CompareOp::LESS, (double)10),
      // QueryCondition(11, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(11, CompareOp::LESS, (double)10),
      // QueryCondition(13, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(13, CompareOp::LESS, (double)10),
      // QueryCondition(15, CompareOp::GREATER_EQUAL, (double)0),
      // QueryCondition(15, CompareOp::LESS, (double)10),
  });

  // BitLSMQuery query({
  //     QueryCondition(1, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(1, CompareOp::LESS, (double)50),
  //     QueryCondition(3, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(3, CompareOp::LESS, (double)50),
  //     QueryCondition(5, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(5, CompareOp::LESS, (double)50),
  //     QueryCondition(7, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(7, CompareOp::LESS, (double)50),
  //     QueryCondition(9, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(9, CompareOp::LESS, (double)50),
  //     QueryCondition(11, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(11, CompareOp::LESS, (double)50),
  //     QueryCondition(13, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(13, CompareOp::LESS, (double)50),
  //     QueryCondition(15, CompareOp::GREATER_EQUAL, (double)0),
  //     QueryCondition(15, CompareOp::LESS, (double)50),
  // });

  // BitLSMQuery query({
  //     QueryCondition(0, CompareOp::EQUAL, "10"),
  //     QueryCondition(2, CompareOp::EQUAL, "10"),
  //     QueryCondition(4, CompareOp::EQUAL, "10"),
  //     QueryCondition(6, CompareOp::EQUAL, "10"),
  // });

  auto x = db.NewIterator(query);
  uint32_t cnt = 0;
  for (x->SeekToFirst(); x->Valid(); x->Next()) {
    cnt += 1;
    // TEST_DumpValue(bit_lsm_options, x->value());
  }
  cout << "total count: " << cnt << "\n";
  cout << "total time: "
       << chrono::duration_cast<chrono::milliseconds>(
              chrono::high_resolution_clock::now() - start_time)
              .count()
       << "ms\n";

  return 0;
}