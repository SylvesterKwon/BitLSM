#include "distribution_generator.h"
#include <cstdint>
#include <random>

// Uniform Distribution Generator
class UniformGenerator : public DistributionGenerator {
  std::mt19937_64 gen;
  std::uniform_int_distribution<uint64_t> dist;

public:
  UniformGenerator(uint64_t cardinality, uint64_t seed = 42)
      : gen(seed), dist(0, cardinality - 1) {}

  uint64_t Next() override { return dist(gen); }
};