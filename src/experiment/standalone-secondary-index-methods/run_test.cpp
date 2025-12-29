#include "eager_updates.h"
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

void test_eager_updates() {
  auto experiment = StandaloneSecondaryIndexExperiment::Create<EagerUpdates>(
      "/scratch/data/eager_updates");
}

int main(const int argc, char* argv[]) {
  // TODO: 테스트 분기 추가
  test_eager_updates();
  return 0;
}
