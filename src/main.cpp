#include <folly/Range.h> // Range 사용을 위해 필요
#include <folly/stats/TDigest.h>
#include <iostream>
#include <vector>

using namespace std;

int main() {
  // 1. TDigest 객체 생성
  folly::TDigest digest(100);

  cout << "[Test] 데이터를 벡터에 모으는 중..." << endl;

  // 2. 데이터를 먼저 벡터에 모읍니다 (Batching)
  vector<double> buffer;
  buffer.reserve(1000); // 성능 최적화
  for (int i = 1; i <= 1000; ++i) {
    buffer.push_back(static_cast<double>(i));
  }

  // 3. 벡터를 한꺼번에 TDigest에 병합합니다.
  // merge는 '새로운 객체'를 반환하므로 다시 digest에 대입해야 합니다.
  // 인자로 folly::Range<const double*> 타입을 요구합니다.
  digest =
      digest.merge(folly::Range<const double*>(buffer.data(), buffer.size()));

  // 4. 통계값 추정
  double p50 = digest.estimateQuantile(0.5);
  double p90 = digest.estimateQuantile(0.9);
  double p99 = digest.estimateQuantile(0.99);

  // 5. 결과 출력
  cout << "--------------------------------" << endl;
  cout << "Count : " << digest.count() << endl;
  cout << "Min   : " << digest.min() << endl;
  cout << "Max   : " << digest.max() << endl;
  cout << "Mean  : " << digest.mean() << endl;
  cout << "--------------------------------" << endl;
  cout << "p50 (Expected ~500.5): " << p50 << endl;
  cout << "p99 (Expected ~990)  : " << p99 << endl;
  cout << "p75 (Expected ~?)  : " << digest.estimateQuantile(0.75) << endl;

  return 0;
}