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

CompiledQuery::CompiledQuery(const BitLSMQuery& query,
                             const BitLSMOptions& options) {
  const ValueLayout layout(options);
  cat_base_ = layout.cat_base;
  for (const OrClause& clause : query.clause_groups) {
    uint32_t begin = static_cast<uint32_t>(preds_.size());
    for (const QueryCondition& cond : clause) {
      Pred p{};
      p.is_cont = layout.is_cont[cond.attr_idx];
      p.op = cond.op;
      p.slot = layout.slot[cond.attr_idx];
      if (p.is_cont) {
        p.dval = std::get<double>(cond.value);
      } else {
        const std::string& s = std::get<std::string>(cond.value);
        p.soff = static_cast<uint32_t>(arena_.size());
        p.slen = static_cast<uint32_t>(s.size());
        arena_ += s;
      }
      preds_.push_back(p);
    }
    clauses_.push_back({begin, static_cast<uint32_t>(preds_.size())});
  }
}

static bool PassOp(CompareOp op, int cmp) {
  switch (op) {
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
  }
  return false;
}

bool CompiledQuery::Eval(rocksdb::Slice value) const {
  const char* base = value.data();
  for (const ClauseRange& c : clauses_) {
    bool clause_pass = false;
    for (uint32_t i = c.begin; i < c.end; ++i) {
      const Pred& p = preds_[i];
      bool ok;
      if (p.is_cont) {
        double v;
        std::memcpy(&v, base + p.slot, sizeof(double));
        switch (p.op) {
          case CompareOp::EQUAL:
            ok = v == p.dval;
            break;
          case CompareOp::GREATER_EQUAL:
            ok = v >= p.dval;
            break;
          case CompareOp::LESS_EQUAL:
            ok = v <= p.dval;
            break;
          case CompareOp::GREATER:
            ok = v > p.dval;
            break;
          case CompareOp::LESS:
            ok = v < p.dval;
            break;
          default:
            ok = false;
        }
      } else {
        uint32_t end;
        std::memcpy(&end, base + p.slot * sizeof(uint32_t), sizeof(uint32_t));
        uint32_t start = 0;
        if (p.slot > 0)
          std::memcpy(&start, base + (p.slot - 1) * sizeof(uint32_t),
                      sizeof(uint32_t));
        std::string_view attr(base + cat_base_ + start, end - start);
        std::string_view want(arena_.data() + p.soff, p.slen);
        ok = PassOp(p.op, attr.compare(want));
      }
      if (ok) {
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