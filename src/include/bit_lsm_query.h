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

// Apply a CompareOp to two same-typed operands (lhs <op> rhs).
template <class T>
inline bool ApplyCompareOp(CompareOp op, const T& lhs, const T& rhs) {
  switch (op) {
    case CompareOp::EQUAL:
      return lhs == rhs;
    case CompareOp::LESS:
      return lhs < rhs;
    case CompareOp::LESS_EQUAL:
      return lhs <= rhs;
    case CompareOp::GREATER:
      return lhs > rhs;
    case CompareOp::GREATER_EQUAL:
      return lhs >= rhs;
  }
  return false;
}

// Query condition. For ORDERED attrs the comparand is a native scalar matching
// the attr's AttrSpec (double for float/double, int64 for signed, uint64 for
// unsigned); for UNORDERED attrs it is the comparand string.
struct QueryCondition {
  uint32_t attr_idx;
  CompareOp op;
  std::variant<int64_t, uint64_t, double, std::string> value;
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
        [](const std::variant<int64_t, uint64_t, double, std::string>& v)
        -> std::string {
      if (std::holds_alternative<std::string>(v))
        return "'" + std::get<std::string>(v) + "'";
      std::ostringstream oss;
      if (std::holds_alternative<int64_t>(v))
        oss << std::get<int64_t>(v);
      else if (std::holds_alternative<uint64_t>(v))
        oss << std::get<uint64_t>(v);
      else
        oss << std::get<double>(v);
      return oss.str();
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
    uint8_t is_ordered;
    CompareOp op;
    int32_t null_bit;  // attr's null-bitmap bit position, or -1 if not nullable
    uint32_t slot;  // ordered: absolute byte offset / unordered: var_end rank
    AttrSpec spec;  // ordered: physical decode spec (width/signed/float)
    int64_t ival;   // ordered comparand; the one matching spec is active
    uint64_t uval;
    double dval;
    uint32_t soff;  // unordered comparand: offset into arena_
    uint32_t slen;
  };
  struct ClauseRange {
    uint32_t begin;
    uint32_t end;
  };
  std::vector<Pred> preds_;
  std::vector<ClauseRange> clauses_;
  uint32_t unordered_base_ = 0;
  uint32_t null_bitmap_bytes_ =
      0;  // leading null bitmap; var_end array follows
  // Owns unordered comparand bytes; preds address it by offset, so copies
  // and moves of CompiledQuery stay valid.
  std::string arena_;
};

}  // namespace bit_lsm