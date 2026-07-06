#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

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
      value;  // double for ordered, string for unordered
};

// A clause is a group of conditions combined with OR.
// Multiple clauses are combined with AND (CNF: Conjunctive Normal Form).
// Example: (a=1 OR a=2) AND (b>=10) → clause_groups = {{a=1, a=2}, {b>=10}}
using OrClause = std::vector<QueryCondition>;

// Full query statement for BitLSM (CNF: AND of OR clauses)
struct BitLSMQuery {
  std::vector<OrClause> clause_groups;

  // Legacy: flat conditions constructor (all AND, each condition becomes its
  // own clause)
  explicit BitLSMQuery() = default;
  explicit BitLSMQuery(std::vector<QueryCondition> conditions) {
    clause_groups.reserve(conditions.size());
    for (auto& c : conditions) clause_groups.push_back({std::move(c)});
  }
  explicit BitLSMQuery(std::vector<OrClause> groups)
      : clause_groups(std::move(groups)) {}

  // Reference row evaluation (tests/oracle); the engine evaluates through
  // CompiledQuery
  bool CheckCondition(rocksdb::Slice slice, const BitLSMOptions& options) const;

  // Structural validation against a schema: rejects empty clauses,
  // out-of-range attr_idx, value/attr type mismatches, and non-EQUAL
  // operators on unordered attributes. OK() means safe to evaluate.
  rocksdb::Status Validate(const BitLSMOptions& options) const;

  // Human-readable query string (e.g., "(a0='2' OR a0='7') AND (a2>='10.5')")
  std::string ToString() const {
    auto op_str = [](CompareOp op) -> const char* {
      switch (op) {
        case CompareOp::EQUAL:
          return "=";
        case CompareOp::LESS:
          return "<";
        case CompareOp::LESS_EQUAL:
          return "<=";
        case CompareOp::GREATER:
          return ">";
        case CompareOp::GREATER_EQUAL:
          return ">=";
      }
      return "?";
    };
    auto val_str =
        [](const std::variant<double, std::string>& v) -> std::string {
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

// A query pre-resolved against a schema: flat predicates with v2 value-format
// slots baked in, so per-row evaluation does no schema lookup, no variant,
// and no offset-table walk. Snapshots the query: the source query/options
// are not referenced after construction. The query must pass Validate()
// against the same options first.
class CompiledQuery {
 public:
  CompiledQuery() = default;
  CompiledQuery(const BitLSMQuery& query, const BitLSMOptions& options);

  // CNF evaluation (AND of OR clauses) of a v2-encoded value.
  // Semantically identical to BitLSMQuery::CheckCondition.
  bool Eval(rocksdb::Slice value) const;

 private:
  struct Pred {
    uint8_t is_cont;
    CompareOp op;
    uint32_t slot;  // cont: absolute byte offset / cat: var_end rank
    double dval;    // ordered comparand
    uint32_t soff;  // unordered comparand: offset into arena_
    uint32_t slen;
  };
  struct ClauseRange {
    uint32_t begin;
    uint32_t end;
  };
  std::vector<Pred> preds_;
  std::vector<ClauseRange> clauses_;
  uint32_t cat_base_ = 0;
  // Owns unordered comparand bytes; preds address it by offset, so copies
  // and moves of CompiledQuery stay valid.
  std::string arena_;
};

}  // namespace bit_lsm