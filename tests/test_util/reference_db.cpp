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
  // 3VL: a NULL attr makes every comparison UNKNOWN, i.e. not a match.
  if (std::holds_alternative<std::monostate>(a)) return false;
  if (options_.attr_specs[cond.attr_idx].role == AttrRole::ORDERED) {
    // Compare in the native domain fixed by the attr's spec.
    if (std::holds_alternative<int64_t>(a))
      return ApplyCompareOp(cond.op, std::get<int64_t>(a),
                            std::get<int64_t>(cond.value));
    if (std::holds_alternative<uint64_t>(a))
      return ApplyCompareOp(cond.op, std::get<uint64_t>(a),
                            std::get<uint64_t>(cond.value));
    return ApplyCompareOp(cond.op, std::get<double>(a),
                          std::get<double>(cond.value));
  }
  return ApplyCompareOp(cond.op, std::get<std::string>(a),
                        std::get<std::string>(cond.value));
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
