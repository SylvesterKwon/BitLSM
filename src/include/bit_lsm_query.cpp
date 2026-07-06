#include <bit_lsm_query.h>

#include "bit_lsm_utils.h"
#include "rocksdb/slice.h"

using namespace std;
using namespace rocksdb;

namespace bit_lsm {

// Evaluate a single condition against a decoded attribute value
static bool EvalCondition(const QueryCondition& cond, AttrType attr_type,
                          AttrView attr_val) {
  if (attr_type == AttrType::CATEGORICAL) {
    std::string_view target_str = std::get<std::string_view>(attr_val);
    const string& query_val = std::get<string>(cond.value);
    int cmp = target_str.compare(query_val);
    switch (cond.op) {
      case CompareOp::EQUAL:
        return cmp == 0;
      case CompareOp::GREATER_EQUAL:
        return cmp >= 0;
      case CompareOp::LESS_EQUAL:
        return cmp <= 0;
      case CompareOp::GREATER:
        return cmp > 0;
      case CompareOp::LESS:
        return cmp < 0;
      default:
        assert(false);
        return false;
    }
  } else {
    double val_double = std::get<double>(attr_val);
    double query_val = std::get<double>(cond.value);
    switch (cond.op) {
      case CompareOp::EQUAL:
        return val_double == query_val;
      case CompareOp::GREATER_EQUAL:
        return val_double >= query_val;
      case CompareOp::LESS_EQUAL:
        return val_double <= query_val;
      case CompareOp::GREATER:
        return val_double > query_val;
      case CompareOp::LESS:
        return val_double < query_val;
      default:
        assert(false);
        return false;
    }
  }
}

// CNF evaluation: all clause_groups (AND) must pass,
// within each group at least one condition (OR) must match.
bool BitLSMQuery::CheckCondition(rocksdb::Slice value_slice,
                                 const BitLSMOptions& options) const {
  if (clause_groups.empty()) return true;

  // Transitional: engine iterators still evaluate through this method, so the
  // layout is rebuilt per call; the compiled-query path replaces it.
  const ValueLayout layout(options);
  std::string_view buffer(value_slice.data(), value_slice.size());

  for (const auto& clause : clause_groups) {
    bool clause_pass = false;
    // Conditions in a clause may reference different attributes (full CNF).
    // NewIterator sorts each clause by attr_idx, so caching the last decoded
    // attribute keeps same-attr clauses at one decode per clause.
    uint32_t cached_idx = UINT32_MAX;
    AttrType cached_type = AttrType::CATEGORICAL;
    AttrView cached_val;
    for (const auto& cond : clause) {
      if (cond.attr_idx != cached_idx) {
        cached_idx = cond.attr_idx;
        cached_type = options.attr_types[cond.attr_idx];
        cached_val = DecodeAttr(layout, buffer, cond.attr_idx);
      }
      if (EvalCondition(cond, cached_type, cached_val)) {
        clause_pass = true;
        break;  // OR short-circuit
      }
    }
    if (!clause_pass) return false;  // AND short-circuit
  }
  return true;
}

rocksdb::Status BitLSMQuery::Validate(const BitLSMOptions& options) const {
  for (size_t ci = 0; ci < clause_groups.size(); ++ci) {
    const OrClause& clause = clause_groups[ci];
    if (clause.empty())
      return rocksdb::Status::InvalidArgument("clause " + std::to_string(ci) +
                                              " is empty");
    for (const QueryCondition& cond : clause) {
      if (cond.attr_idx >= options.attr_types.size())
        return rocksdb::Status::InvalidArgument(
            "attr_idx " + std::to_string(cond.attr_idx) +
            " out of range (attr_num=" +
            std::to_string(options.attr_types.size()) + ")");
      AttrType type = options.attr_types[cond.attr_idx];
      if (type == AttrType::CONTINUOUS) {
        if (!std::holds_alternative<double>(cond.value))
          return rocksdb::Status::InvalidArgument(
              "attr " + std::to_string(cond.attr_idx) +
              " is CONTINUOUS but value is not double");
      } else if (type == AttrType::CATEGORICAL) {
        if (!std::holds_alternative<std::string>(cond.value))
          return rocksdb::Status::InvalidArgument(
              "attr " + std::to_string(cond.attr_idx) +
              " is CATEGORICAL but value is not string");
        if (cond.op != CompareOp::EQUAL)
          return rocksdb::Status::InvalidArgument(
              "categorical attr " + std::to_string(cond.attr_idx) +
              " supports only EQUAL");
      }
    }
  }
  return rocksdb::Status::OK();
}
}  // namespace bit_lsm