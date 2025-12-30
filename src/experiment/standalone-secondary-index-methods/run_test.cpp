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
  experiment->Insert(Slice("pk2"), Slice("sk1,value2"));
  experiment->Insert(Slice("pk1"), Slice("sk1,value1"));

  vector<pair<string, PinnableSlice>> results;
  experiment->GetBySecondaryIndex(Slice("sk1"), &results);
  cout << "test1: \n";
  for (auto& [k, v] : results) {
    cout << "{" << k << ", " << v.ToStringView() << "},";
  }
  cout << "\n";

  experiment->Insert(Slice("pk3"), Slice("sk2,value3"));
  experiment->Insert(Slice("pk4"), Slice("sk1,value4"));

  experiment->GetBySecondaryIndex(Slice("sk1"), &results);
  cout << "test2: \n";
  for (auto& [k, v] : results) {
    cout << "{" << k << ", " << v.ToStringView() << "},";
  }
  cout << "\n";
}

void test_eager_updates() {
  auto experiment = StandaloneSecondaryIndexExperiment::Create<EagerUpdates>(
      "/scratch/data/eager_updates");
  simple_test(experiment.get());
}

void test_lazy_updates() {
  auto experiment = StandaloneSecondaryIndexExperiment::Create<LazyUpdates>(
      "/scratch/data/lazy_updates");
  simple_test(experiment.get());
}

int main(const int argc, char* argv[]) {
  // TODO: 테스트 분기 추가
  // test_eager_updates();
  test_lazy_updates();
  return 0;
}
