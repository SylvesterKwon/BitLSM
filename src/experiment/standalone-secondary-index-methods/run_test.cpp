#include "composite_keys.h"
#include "eager_updates.h"
#include "lazy_updates.h"
#include "rocksdb/slice.h"
#include <cstring>
#include <iostream>

using namespace std;
using namespace rocksdb;

enum TestType {
  // TODO: 테스트 유형 정해보기. SK distribution은 어떻게 보여줄지?
};

void generate_workload() {
  // TODO: Generate workload by test type
}

void simple_test(StandaloneSecondaryIndexExperiment* experiment) {
  experiment->Insert(Slice("pk2"), Slice("sk1_1,sk2_2,value2"));
  experiment->Insert(Slice("pk1"), Slice("sk1_1,sk2_1,value1"));
  experiment->Insert(Slice("pk4"), Slice("sk1_2,sk2_2,value4"));
  experiment->Insert(Slice("pk3"), Slice("sk1_2,sk2_1,value3"));

  vector<pair<string, PinnableSlice>> results;
  experiment->GetBySecondaryIndex(Slice("sk1_1"), 0, &results);
  cout << "test1: \n";
  for (auto& [k, v] : results) {
    cout << "{" << k << ", " << v.ToStringView() << "},";
  }
  cout << "\n";
  // expected output: {pk1, sk1_1,sk2_1,value1},{pk2, sk1_1,sk2_2,value2},

  experiment->GetBySecondaryIndex(Slice("sk2_2"), 1, &results);
  cout << "test2: \n";
  for (auto& [k, v] : results) {
    cout << "{" << k << ", " << v.ToStringView() << "},";
  }
  cout << "\n";
  // expected output: {pk2, sk1_1,sk2_2,value2},{pk4, sk1_2,sk2_2,value4},
}

void test_eager_updates() {
  auto experiment = StandaloneSecondaryIndexExperiment::Create<EagerUpdates>(
      "/scratch/data/eager_updates", 2);
  simple_test(experiment.get());
}

void test_lazy_updates() {
  auto experiment = StandaloneSecondaryIndexExperiment::Create<LazyUpdates>(
      "/scratch/data/lazy_updates", 2);
  simple_test(experiment.get());
}

void test_composite_keys() {
  auto experiment = StandaloneSecondaryIndexExperiment::Create<CompositeKeys>(
      "/scratch/data/composite_keys", 2);
  simple_test(experiment.get());
}

int main(const int argc, char* argv[]) {
  // TODO: 테스트 분기 추가
  // test_eager_updates();
  // test_lazy_updates();
  test_composite_keys();
  return 0;
}
