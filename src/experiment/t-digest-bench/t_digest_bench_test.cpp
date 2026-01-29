#include <chrono>
#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <iostream>
#include <vector>

#include "uniform_generator.h"

using namespace std;

const int n = 1e6; // Input size
const int q = 1e2; // Query size

vector<double> buffer;

chrono::_V2::system_clock::time_point start_time, end_time;
long elapsed_time() {
  return chrono::duration_cast<chrono::microseconds>(end_time - start_time)
      .count();
}

void tdigest_bench() {
  start_time = chrono::high_resolution_clock::now();

  folly::TDigest digest(1000);
  digest =
      digest.merge(folly::Range<const double*>(buffer.data(), buffer.size()));

  end_time = chrono::high_resolution_clock::now();
  cout << "phase 1: " << elapsed_time() << "\n";

  for (uint32_t i = 1; i <= q; ++i) {
    // 2. 분위수 계산
    double quantile = (double)i / (double)q;

    // 3. 인덱스 계산 (단순 절삭 사용)
    digest.estimateQuantile(quantile);
  }

  end_time = chrono::high_resolution_clock::now();
  cout << "phase 2: " << elapsed_time() << "\n";
}

void vector_bench() {
  start_time = chrono::high_resolution_clock::now();

  // 1. Sort buffer
  vector<double> buffer_cpy = buffer;
  sort(buffer_cpy.begin(), buffer_cpy.end());

  end_time = chrono::high_resolution_clock::now();
  cout << "phase 1: " << elapsed_time() << "\n";

  for (uint32_t i = 1; i <= q; ++i) {
    // 2. 분위수 계산
    double quantile = (double)i / (double)q;

    // 3. 인덱스 계산 (단순 절삭 사용)
    int index = static_cast<int>(quantile * (buffer_cpy.size() - 1));
    double res = buffer_cpy[index];
  }

  end_time = chrono::high_resolution_clock::now();
  cout << "phase 2: " << elapsed_time() << "\n";
}

int main() {
  UniformGenerator ug(n);

  // 1. 랜덤 double vector 생성
  for (int i = 0; i < n; ++i) {
    buffer.push_back(static_cast<double>(ug.Next()));
  }
  cout << "[ T-Digest Benchmark ]\n";
  tdigest_bench();
  cout << "[ Sort Benchmark ]\n";
  vector_bench();

  return 0;
}