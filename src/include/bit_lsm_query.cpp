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
                                 const BitLSMOptions& options) {
  if (clause_groups.empty()) return true;

  std::string_view buffer(value_slice.data(), value_slice.size());

  for (const auto& clause : clause_groups) {
    bool clause_pass = false;
    AttrType attr_type = options.attr_types[clause[0].attr_idx];
    AttrView attr_val = DecodeAttr(attr_type, buffer, clause[0].attr_idx);
    for (const auto& cond : clause) {
      if (EvalCondition(cond, attr_type, attr_val)) {
        clause_pass = true;
        break;  // OR short-circuit
      }
    }
    if (!clause_pass) return false;  // AND short-circuit
  }
  return true;
}
}  // namespace bit_lsm