#pragma once

#include "bit_lsm_option.h"
#include "rocksdb/slice.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace bit_lsm {

// Compare Operator
enum class CompareOp {
  EQUAL,
  LESS_EQUAL,
  GREATER_EQUAL,
  LESS,
  GREATER,
};

// Query condition
struct QueryCondition {
  uint32_t attr_idx;
  CompareOp op;
  std::variant<double, std::string>
      value; // double for continuous, string for categorical
};

// A clause is a group of conditions combined with OR.
// Multiple clauses are combined with AND (CNF: Conjunctive Normal Form).
// Example: (a=1 OR a=2) AND (b>=10) → clause_groups = {{a=1, a=2}, {b>=10}}
using OrClause = std::vector<QueryCondition>;

// Full query statement for BitLSM (CNF: AND of OR clauses)
struct BitLSMQuery {
  std::vector<OrClause> clause_groups;

  // Legacy: flat conditions constructor (all AND, each condition becomes its own
  // clause)
  explicit BitLSMQuery() = default;
  explicit BitLSMQuery(std::vector<QueryCondition> conditions) {
    clause_groups.reserve(conditions.size());
    for (auto& c : conditions)
      clause_groups.push_back({std::move(c)});
  }
  explicit BitLSMQuery(std::vector<OrClause> groups)
      : clause_groups(std::move(groups)) {}

  // Validate given slice with given query condition & options
  bool CheckCondition(rocksdb::Slice slice, const BitLSMOptions& options);

  // Human-readable query string (e.g., "(a0='2' OR a0='7') AND (a2>='10.5')")
  std::string ToString() const {
    auto op_str = [](CompareOp op) -> const char* {
      switch (op) {
      case CompareOp::EQUAL: return "=";
      case CompareOp::LESS: return "<";
      case CompareOp::LESS_EQUAL: return "<=";
      case CompareOp::GREATER: return ">";
      case CompareOp::GREATER_EQUAL: return ">=";
      }
      return "?";
    };
    auto val_str = [](const std::variant<double, std::string>& v) -> std::string {
      if (std::holds_alternative<double>(v)) {
        std::ostringstream oss;
        oss << std::get<double>(v);
        return oss.str();
      }
      return "'" + std::get<std::string>(v) + "'";
    };

    std::ostringstream out;
    for (size_t i = 0; i < clause_groups.size(); ++i) {
      const auto& clause = clause_groups[i];
      if (i > 0) out << " AND ";
      if (clause.size() > 1) out << "(";
      for (size_t j = 0; j < clause.size(); ++j) {
        if (j > 0) out << " OR ";
        const auto& c = clause[j];
        out << "a" << c.attr_idx << op_str(c.op) << val_str(c.value);
      }
      if (clause.size() > 1) out << ")";
    }
    return out.str();
  }
};

} // namespace bit_lsm