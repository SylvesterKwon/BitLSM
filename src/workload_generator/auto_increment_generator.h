#include "distribution_generator.h"
#include <cstdint>

// AutoIncrement Distribution Generator
class AutoIncrementGenerator : public DistributionGenerator {
private:
  uint64_t auto_increment_counter_ = 0;

public:
  AutoIncrementGenerator(uint64_t auto_increment_counter = 0)
      : auto_increment_counter_(auto_increment_counter) {}

  uint64_t Next() override { return auto_increment_counter_++; }
};