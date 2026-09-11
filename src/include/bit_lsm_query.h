#pragma once

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
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

// Query condition. The comparand alternative follows the attr's physical
// type: int64 for kInt, uint64 for kUint, double for kFloat, string for
// kBinary/kVarBinary.
struct QueryCondition {
  using value_type = std::variant<int64_t, uint64_t, double, std::string>;
  uint32_t attr_idx;
  CompareOp op;
  value_type value;
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
  // out-of-range attr_idx, comparand/physical-type mismatches, and non-EQUAL
  // operators on kEquality attributes. OK() means safe to evaluate.
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

// Interval on the SABI byte domain (memcmp order). "" is the domain minimum,
// so lo == "" closed means unbounded below; the domain has no maximum, so hi
// carries an explicit unbounded flag. Both ends carry an open flag (bytes
// have no predecessor and a successor -- s + '\0' -- with no room between,
// so strict bounds stay on the comparand instead of stepping). Empty is
// absorbing under Intersect.
struct ByteInterval {
  std::string lo;
  bool lo_open = false;
  std::string hi;
  bool hi_open = false;
  bool hi_unbounded = true;

  bool Empty() const {
    if (hi_unbounded) return false;
    const int c = lo.compare(hi);
    return c > 0 || (c == 0 && (lo_open || hi_open));
  }
  void Intersect(const ByteInterval& o) {
    const int lc = o.lo.compare(lo);
    if (lc > 0) {
      lo = o.lo;
      lo_open = o.lo_open;
    } else if (lc == 0) {
      lo_open = lo_open || o.lo_open;
    }
    if (o.hi_unbounded) return;
    if (hi_unbounded) {
      hi = o.hi;
      hi_open = o.hi_open;
      hi_unbounded = false;
      return;
    }
    const int c = o.hi.compare(hi);
    if (c < 0) {
      hi = o.hi;
      hi_open = o.hi_open;
    } else if (c == 0) {
      hi_open = hi_open || o.hi_open;
    }
  }
  static ByteInterval FromOp(CompareOp op, std::string_view bytes) {
    ByteInterval w;
    switch (op) {
      case CompareOp::EQUAL:
        w.lo.assign(bytes);
        w.hi.assign(bytes);
        w.hi_unbounded = false;
        break;
      case CompareOp::GREATER_EQUAL:
        w.lo.assign(bytes);
        break;
      case CompareOp::GREATER:
        w.lo.assign(bytes);
        w.lo_open = true;
        break;
      case CompareOp::LESS_EQUAL:
        w.hi.assign(bytes);
        w.hi_unbounded = false;
        break;
      case CompareOp::LESS:
        w.hi.assign(bytes);
        w.hi_open = true;
        w.hi_unbounded = false;
        break;
    }
    return w;
  }
};

// A query with comparands pre-encoded into the SABI byte domain: a
// ByteInterval for kRange attrs (numeric comparands become 8-byte okeys, so
// one interval type serves ints, floats and strings alike), opaque bytes for
// kEquality equality. Built once per query; EncodeQuery folds the operator
// into `win` and merges same-attr single-condition clauses by intersection,
// so a BETWEEN-shaped CNF reaches every consumer as one interval.
struct SABICondition {
  uint32_t attr_idx;
  ByteInterval win;   // active when the attr is kRange
  std::string bytes;  // active when the attr is kEquality (always EQUAL)
};
using SABIOrClause = std::vector<SABICondition>;
struct SABIQuery {
  std::vector<SABIOrClause> clause_groups;
  // Provably matchless by interval algebra alone (a contradiction like
  // x > 5 AND x < 3, or a strict bound off the okey domain edge). Distinct
  // from empty clause_groups, which means full scan.
  bool unsat = false;
};

// Standalone adapter: resolves each comparand against its AttrSpec. The query
// must pass Validate(options) first.
SABIQuery EncodeQuery(const BitLSMQuery& q, const BitLSMOptions& options);

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
    uint8_t is_numeric;
    CompareOp op;
    int32_t null_bit;  // attr's null-bitmap bit position, or -1 if not nullable
    uint32_t slot;     // fixed: absolute byte offset / kVarBinary: var_end rank
    AttrSpec spec;     // physical type / width
    int64_t ival;      // numeric comparand; the one matching spec is active
    uint64_t uval;
    double dval;
    uint32_t soff;  // binary comparand: offset into arena_
    uint32_t slen;
  };
  struct ClauseRange {
    uint32_t begin;
    uint32_t end;
  };
  std::vector<Pred> preds_;
  std::vector<ClauseRange> clauses_;
  uint32_t variable_base_ = 0;
  uint32_t null_bitmap_bytes_ =
      0;  // leading null bitmap; var_end array follows
  // Owns binary comparand bytes; preds address it by offset, so copies and
  // moves of CompiledQuery stay valid.
  std::string arena_;
};

}  // namespace bit_lsm