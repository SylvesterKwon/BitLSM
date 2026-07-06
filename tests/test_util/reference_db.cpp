#include "test_util/reference_db.h"

namespace bit_lsm {

void ReferenceDB::Put(const std::string& key, std::vector<Attr> attrs,
                      std::string payload) {
  live_[key] = Record{std::move(attrs), std::move(payload)};  // latest wins
}

void ReferenceDB::Delete(const std::string& key) { live_.erase(key); }

void ReferenceDB::Clear() { live_.clear(); }

bool ReferenceDB::MatchCondition(const QueryCondition& cond,
                                 const std::vector<Attr>& attrs) const {
  const Attr& a = attrs[cond.attr_idx];
  if (options_.attr_specs[cond.attr_idx].role == AttrType::ORDERED) {
    double lhs = std::get<double>(a);
    double rhs = std::get<double>(cond.value);
    switch (cond.op) {
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
  } else {
    const std::string& lhs = std::get<std::string>(a);
    const std::string& rhs = std::get<std::string>(cond.value);
    switch (cond.op) {
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
  }
  return false;
}

bool ReferenceDB::Match(const BitLSMQuery& query,
                        const std::vector<Attr>& attrs) const {
  // AND of OR-clauses: every clause needs at least one satisfied condition.
  for (const OrClause& clause : query.clause_groups) {
    bool clause_ok = false;
    for (const QueryCondition& cond : clause) {
      if (MatchCondition(cond, attrs)) {
        clause_ok = true;
        break;
      }
    }
    if (!clause_ok) return false;
  }
  return true;  // empty query (no clauses) matches everything
}

std::map<std::string, Record> ReferenceDB::ExpectedResult(
    const BitLSMQuery& query) const {
  std::map<std::string, Record> out;
  for (const auto& [key, rec] : live_) {
    if (Match(query, rec.attrs)) out.emplace(key, rec);
  }
  return out;
}

}  // namespace bit_lsm
