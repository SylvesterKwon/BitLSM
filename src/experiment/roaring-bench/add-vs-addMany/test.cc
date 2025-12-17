#include <chrono>
#include <iostream>

#include "roaring.hh"

using namespace std;
using namespace roaring;
using namespace roaring::api;

chrono::_V2::system_clock::time_point start_time, end_time;
chrono::milliseconds ms_duration;

struct TestResult {
  long eplased_time;
  uint32_t roaring_size;
};

long eplased_time() {
  return chrono::duration_cast<chrono::milliseconds>(end_time - start_time)
      .count();
}

TestResult test_add(uint32_t bitset_size, double p) {
  srand(time(NULL));
  Roaring r;
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

  return TestResult(eplased_time(), r.getSizeInBytes());
}

TestResult test_add_many(uint32_t bitset_size, double p) {
  srand(time(NULL));
  Roaring r;
  vector<bool> original_bitset(bitset_size);
  for (uint32_t i = 0; i < bitset_size; i++) {
    original_bitset[i] = ((double)rand() / RAND_MAX) < p;
  }

  start_time = chrono::high_resolution_clock::now();
  vector<uint32_t> indices;
  for (uint32_t i = 0; i < bitset_size; i++) {
    if (original_bitset[i])
      indices.push_back(i);
  }
  r.addMany(indices.size(), indices.data());
  end_time = chrono::high_resolution_clock::now();

  return TestResult(eplased_time(), r.getSizeInBytes());
}

int main() {
  double p = 0.5;
  const uint32_t iteration_count = 5;
  uint32_t bitset_size = (uint32_t)1e7;

  long elapsed_time_sum = 0;
  uint32_t roaring_size_sum = 0;
  for (int i = 0; i < iteration_count; i++) {
    TestResult tr = test_add(bitset_size, p);
    elapsed_time_sum += tr.eplased_time;
    roaring_size_sum += tr.roaring_size;
  }
  cout << "[TEST RESULT]\n";
  cout << "p: " << p << "\n";
  cout << "bitset_size: " << bitset_size << "\n";
  cout << "iteration_count: " << iteration_count << "\n";
  cout << "avg. elapsed time: " << (double)elapsed_time_sum / iteration_count
       << "\n";
  cout << "avg. roaring size: " << roaring_size_sum / iteration_count << "\n";
  return EXIT_SUCCESS;
}
