#pragma once
#include <cstdint>

// Interface for Distribution Generator
class DistributionGenerator {
public:
  virtual ~DistributionGenerator() = default;
  virtual uint64_t Next() = 0;
};
