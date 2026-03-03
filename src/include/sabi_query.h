#pragma once

#include "rocksdb/slice.h"
#include "sabi.h"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace bitmap_index {

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
  uint32_t sk_idx;
  CompareOp op;
  std::variant<double, std::string>
      value; // double for continuous, string for categorical
};

// Full query statement for SABI
struct SABIQuery {
  std::vector<QueryCondition> conditions;
  bool CheckCondition(rocksdb::Slice slice, SABIOptions options);
};

} // namespace bitmap_index