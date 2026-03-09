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

  // Write test
  // fill_kvp(&db, 1e7, 16, 32, 42, true);

  // Statistics
  db.Statistics();

  // query samples
  BitLSMQuery query(
      {QueryCondition(1, CompareOp::GREATER_EQUAL, (double)24.0002),
       QueryCondition(1, CompareOp::LESS_EQUAL, (double)24.0003)});

  auto x = db.NewIterator(query);
  uint32_t cnt = 0;
  for (x->SeekToFirst(); x->Valid(); x->Next()) {
    cout << x->key().ToStringView() << ": ";
    x->TEST_DumpValue(x->value());
    cnt += 1;
  }
  cout << "total #: " << cnt << "\n";

  return 0;
}
