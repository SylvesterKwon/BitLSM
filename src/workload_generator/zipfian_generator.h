#include "distribution_generator.h"
#include <cstdint>
#include <memory>
#include <random>

// FNV Hash
inline uint64_t FNVHash64(uint64_t val, uint32_t seed = 42) {
  constexpr uint64_t FNV_OFFSET_BASIS_64 = 0xcbf29ce484222325ULL;
  constexpr uint64_t FNV_PRIME_64 = 1099511628211ULL;

  uint64_t hash = FNV_OFFSET_BASIS_64 ^ seed;
  for (int i = 0; i < 8; i++) {
    uint64_t octet = val & 0xff;
    val >>= 8;
    hash ^= octet;
    hash *= FNV_PRIME_64;
  }
  return hash;
}

// Zipfian Generator
class ZipfianGenerator : public DistributionGenerator {
private:
  uint64_t cardinality;
  double theta;
  double alpha, zetan, eta, zeta2theta;

  std::mt19937_64 gen;
  std::uniform_real_distribution<double> dist;

  // Calculate Zeta (1/1^theta + ... + 1/n^theta)
  double Zeta(uint64_t n, double thetaVal) {
    double sum = 0;
    for (uint64_t i = 0; i < n; i++) {
      sum += 1.0 / pow(i + 1, thetaVal);
    }
    return sum;
  }

  // For performance, use Zeta approximation fomular for big N
  // Using Euler-Maclaurin formula
  double ZetaApprox(uint64_t n, double thetaVal) {
    // Handle case when theta is near 1.0. Prevent divided by zero
    if (abs(thetaVal - 1.0) < 1e-9) {
      return log(n + 1);
    }

    // Integral approximation for the general case
    return (pow(n, 1.0 - thetaVal) - 1.0) / (1.0 - thetaVal) + 1.0;
  }

public:
  ZipfianGenerator(uint64_t cardinality, double theta = 0.99,
                   uint32_t seed = 42)
      : cardinality(cardinality), theta(theta), dist(0.0, 1.0) {
    gen.seed(seed);

    zeta2theta = Zeta(2, theta);
    zetan = ZetaApprox(cardinality, theta);
    alpha = 1.0 / (1.0 - theta);
    eta = (1 - pow(2.0 / cardinality, 1 - theta)) / (1 - zeta2theta / zetan);
  }

  uint64_t Next() override {
    double u = dist(gen);
    double uz = u * zetan;

    if (uz < 1.0)
      return 0;
    if (uz < 1.0 + pow(0.5, theta))
      return 1;

    return static_cast<uint64_t>(cardinality * pow(eta * u - eta + 1, alpha));
  }
};

// Scrambled Zipfian Generator
class ScrambledZipfianGenerator : public DistributionGenerator {
private:
  std::unique_ptr<ZipfianGenerator> zipf_gen;
  uint64_t cardinality;
  uint32_t seed;

public:
  ScrambledZipfianGenerator(uint64_t cardinality, double theta = 0.99,
                            uint32_t seed = 42)
      : cardinality(cardinality), seed(seed) {
    zipf_gen = std::make_unique<ZipfianGenerator>(cardinality, theta, seed);
  }

  uint64_t Next() override {
    uint64_t rank = zipf_gen->Next();
    return (FNVHash64(rank, seed) % cardinality);
  }
};