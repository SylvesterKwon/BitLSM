#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

// Build a TDigest over `values` the same way SABIBuilder does.
folly::TDigest BuildDigest(const std::vector<double>& values, size_t max_size) {
  folly::TDigest digest(max_size);
  return digest.merge(
      folly::Range<const double*>(values.data(), values.size()));
}

// Workload: 1000 evenly-spaced values [0, 999] merged into a TDigest, then the
//           N+1 quantile boundaries SABIBuilder computes are read back.
// Threat: the minimal folly::Range / shim layer silently corrupts the merge so
//         quantile estimates are wrong or non-monotonic.
TEST(TDigestVendored, MonotonicBoundariesAndEndpoints) {
  std::vector<double> values;
  values.reserve(1000);
  for (int i = 0; i < 1000; ++i) values.push_back(static_cast<double>(i));

  folly::TDigest digest = BuildDigest(values, /*max_size=*/100);

  const int bins = 10;
  std::vector<double> boundaries(bins + 1);
  for (int j = 0; j <= bins; ++j) {
    boundaries[j] = digest.estimateQuantile(static_cast<double>(j) / bins);
  }

  // Endpoints are exact min/max.
  EXPECT_DOUBLE_EQ(boundaries.front(), 0.0);
  EXPECT_DOUBLE_EQ(boundaries.back(), 999.0);
  // Boundaries are non-decreasing.
  for (int j = 1; j <= bins; ++j) {
    EXPECT_LE(boundaries[j - 1], boundaries[j]) << "at j=" << j;
  }
  // The median of a uniform 0..999 sample is ~499.5 (loose tolerance: TDigest
  // is approximate, but must be close for this many points).
  EXPECT_NEAR(digest.estimateQuantile(0.5), 499.5, 5.0);
}

// Workload: a degenerate all-identical input (every value 7.0).
// Threat: empty/degenerate handling in the shimmed merge path divides by zero
//         or returns NaN instead of the single value.
TEST(TDigestVendored, AllIdenticalValues) {
  std::vector<double> values(50, 7.0);
  folly::TDigest digest = BuildDigest(values, /*max_size=*/100);

  EXPECT_DOUBLE_EQ(digest.estimateQuantile(0.0), 7.0);
  EXPECT_DOUBLE_EQ(digest.estimateQuantile(0.5), 7.0);
  EXPECT_DOUBLE_EQ(digest.estimateQuantile(1.0), 7.0);
  EXPECT_FALSE(std::isnan(digest.mean()));
  EXPECT_DOUBLE_EQ(digest.mean(), 7.0);
}

}  // namespace
