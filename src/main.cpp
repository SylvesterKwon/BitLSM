#include "bit_lsm.h"
#include "sabi.h"
#include "utils.h"
#include <cstring>

using namespace std;
using namespace rocksdb;
using namespace roaring;
using namespace bit_lsm;

int main(const int argc, char* argv[]) {
  string db_path = "/scratch/data/user_defined_index_test";

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
  // fill_kvp(&db, 1e7, 16, 32, 42, true);
  // return 0;

  // Statistics
  // db.Statistics();

  // query samples
  chrono::_V2::system_clock::time_point start_time =
      chrono::high_resolution_clock::now();
  for (uint32_t i = 0; i < 10; i++) {
    BitLSMQuery query(
        {QueryCondition(9, CompareOp::GREATER_EQUAL, (double)i * 10),
         QueryCondition(9, CompareOp::LESS_EQUAL, (double)(i + 1) * 10)});
    auto x = db.NewIterator(query);
    uint32_t cnt = 0;
    for (x->SeekToFirst(); x->Valid(); x->Next()) {
      cnt += 1;
    }
    cout << i << "\n";
  }

  cout << "total:"
       << chrono::duration_cast<chrono::milliseconds>(
              chrono::high_resolution_clock::now() - start_time)
              .count()
       << "ms elpased\n";

  return 0;
}
