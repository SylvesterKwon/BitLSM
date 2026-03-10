#pragma once

#include "bit_lsm_option.h"
#include "rocksdb/slice.h"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace bit_lsm {

// Compare Operator
enum class CompareOp {
  EQUAL,
  LESS_EQUAL,
  GREATER_EQUAL,
  // LESS,
  // GREATER,
};

// Query condition
struct QueryCondition {
  uint32_t attr_idx;
  CompareOp op;
  std::variant<double, std::string>
      value; // double for continuous, string for categorical
};

// Full query statement for BitLSM
struct BitLSMQuery {
  std::vector<QueryCondition> conditions;
  // Validate given slice with given query condition & options
  bool CheckCondition(rocksdb::Slice slice, const BitLSMOptions& options);
};

} // namespace bit_lsm