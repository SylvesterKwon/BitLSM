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

string db_path = "/scratch/data/test-deletion-support";
uint32_t num_threads = 4;
int main(const int argc, char* argv[]) {
  // test {# of kvp, payload bytes, rho}

  // 테스트용 옵션
  BitLSMOptions bit_lsm_options;
  bit_lsm_options.rho = 0.1;
  bit_lsm_options.attr_num = 4;
  for (uint32_t i = 0; i < bit_lsm_options.attr_num; ++i) {
    if (i % 2 == 0)
      bit_lsm_options.attr_types.push_back(AttrType::CATEGORICAL);
    else
      bit_lsm_options.attr_types.push_back(AttrType::CONTINUOUS);
  }

  BitLSM db(db_path, bit_lsm_options);

  // Write test
  fill_kvp_multi_thread(&db, num_threads, 1e7, bit_lsm_options, 32, 42, true);
  // return 0;
  for (uint32_t i = 0; i < 1e7; i += 2) {
    db.Delete(to_string(i));
  }
  for (uint32_t i = 0; i < 1e7; i += 3) {
    db.Delete(to_string(i));
  }

  BitLSMQuery query({
      QueryCondition(1, CompareOp::LESS_EQUAL, (double)100),
  });

  auto x = db.NewIterator(query);
  uint32_t cnt = 0;
  for (x->SeekToFirst(); x->Valid(); x->Next()) {
    cnt += 1;
    // TEST_DumpValue(bit_lsm_options, x->value());
  }
  cout << "Count: " << cnt << "\n";

  return 0;
}