#include <chrono>
#include <iostream>

#include "roaring.hh"

using namespace std;
using namespace roaring;
using namespace roaring::api;

chrono::_V2::system_clock::time_point start_time, end_time;
chrono::milliseconds ms_duration;

struct TestResult {
  long eplased_time_add;
  long eplased_time_optimize;
  uint32_t roaring_size_before_optimize;
  uint32_t roaring_size_after_optimize;
};

long eplased_time() {
  return chrono::duration_cast<chrono::milliseconds>(end_time - start_time)
      .count();
}

TestResult test_run_optimize(uint32_t bitset_size, double p) {
  srand(time(NULL));
  Roaring r;
  TestResult tr;
  vector<bool> original_bitset(bitset_size);
  for (uint32_t i = 0; i < bitset_size; i++) {
    original_bitset[i] = ((double)rand() / RAND_MAX) < p;
  }

  start_time = chrono::high_resolution_clock::now();
  for (uint32_t i = 0; i < bitset_size; i++) {
    if (original_bitset[i])
      r.add(i);
  }
  end_time = chrono::high_resolution_clock::now();
  tr.roaring_size_before_optimize = r.getSizeInBytes();
  tr.eplased_time_add = eplased_time();

  start_time = chrono::high_resolution_clock::now();
  bool optimize_result = r.runOptimize();
  end_time = chrono::high_resolution_clock::now();
  tr.roaring_size_after_optimize = r.getSizeInBytes();
  tr.eplased_time_optimize = eplased_time();

  return tr;
}

int main() {
  double p = 0.9;
  const uint32_t iteration_count = 5;
  uint32_t bitset_size = (uint32_t)1e7;

  long roaring_size_before_optimize_sum = 0;
  long roaring_size_after_optimize_sum = 0;
  uint32_t eplased_time_add_sum = 0;
  uint32_t eplased_time_optimize_sum = 0;
  for (int i = 0; i < iteration_count; i++) {
    TestResult tr = test_run_optimize(bitset_size, p);
    roaring_size_before_optimize_sum += tr.roaring_size_before_optimize;
    roaring_size_after_optimize_sum += tr.roaring_size_after_optimize;
    eplased_time_add_sum += tr.eplased_time_add;
    eplased_time_optimize_sum += tr.eplased_time_optimize;
  }
  cout << "[TEST RESULT]\n";
  cout << "p: " << p << "\n";
  cout << "bitset_size: " << bitset_size << "\n";
  cout << "iteration_count: " << iteration_count << "\n";
  cout << "avg. roaring_size_before_optimize: "
       << roaring_size_before_optimize_sum / iteration_count << "\n";
  cout << "avg. roaring_size_after_optimize: "
       << roaring_size_after_optimize_sum / iteration_count << "\n";
  cout << "avg. eplased_time_add: " << eplased_time_add_sum / iteration_count
       << "\n";
  cout << "avg. eplased_time_optimize: "
       << eplased_time_optimize_sum / iteration_count << "\n";
  return EXIT_SUCCESS;
}
