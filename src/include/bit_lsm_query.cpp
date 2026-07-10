#include <bit_lsm_query.h>

#include "bit_lsm_encoding.h"
#include "bit_lsm_utils.h"
#include "rocksdb/slice.h"

using namespace std;
using namespace rocksdb;

namespace bit_lsm {

// Evaluate a single condition against a decoded attribute value
static bool EvalCondition(const QueryCondition& cond, AttrRole attr_type,
                          AttrView attr_val) {
  // 3VL: a NULL attr makes every comparison UNKNOWN, i.e. not a match.
  if (std::holds_alternative<std::monostate>(attr_val)) return false;
  if (attr_type == AttrRole::UNORDERED) {
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
    // ORDERED: compare in the attr's native domain (int64/uint64/double). The
    // decoded value and the comparand share the alternative fixed by the spec.
    if (std::holds_alternative<int64_t>(attr_val))
      return ApplyCompareOp(cond.op, std::get<int64_t>(attr_val),
                            std::get<int64_t>(cond.value));
    if (std::holds_alternative<uint64_t>(attr_val))
      return ApplyCompareOp(cond.op, std::get<uint64_t>(attr_val),
                            std::get<uint64_t>(cond.value));
    return ApplyCompareOp(cond.op, std::get<double>(attr_val),
                          std::get<double>(cond.value));
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
    AttrRole cached_type = AttrRole::UNORDERED;
    AttrView cached_val;
    for (const auto& cond : clause) {
      if (cond.attr_idx != cached_idx) {
        cached_idx = cond.attr_idx;
        cached_type = options.attr_specs[cond.attr_idx].role;
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
  unordered_base_ = layout.unordered_base;
  null_bitmap_bytes_ = layout.null_bitmap_bytes;
  for (const OrClause& clause : query.clause_groups) {
    uint32_t begin = static_cast<uint32_t>(preds_.size());
    for (const QueryCondition& cond : clause) {
      Pred p{};
      p.is_ordered = layout.is_ordered[cond.attr_idx];
      p.op = cond.op;
      p.null_bit = layout.null_bit[cond.attr_idx];
      p.slot = layout.slot[cond.attr_idx];
      if (p.is_ordered) {
        p.spec = layout.specs[cond.attr_idx];
        if (p.spec.is_float)
          p.dval = std::get<double>(cond.value);
        else if (p.spec.is_signed)
          p.ival = std::get<int64_t>(cond.value);
        else
          p.uval = std::get<uint64_t>(cond.value);
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
      if (p.null_bit >= 0 && IsNullBitSet(base, p.null_bit)) {
        // 3VL: NULL attr → comparison is UNKNOWN, treated as no-match.
        ok = false;
      } else if (p.is_ordered) {
        // Fast path: 8-byte double (the common experiment schema) compares
        // straight from the slot with no variant construction.
        if (p.spec.is_float && p.spec.width == 8) {
          double v;
          std::memcpy(&v, base + p.slot, sizeof(double));
          ok = ApplyCompareOp(p.op, v, p.dval);
        } else {
          AttrView v = DecodeOrdered(base + p.slot, p.spec);
          if (p.spec.is_float)
            ok = ApplyCompareOp(p.op, std::get<double>(v), p.dval);
          else if (p.spec.is_signed)
            ok = ApplyCompareOp(p.op, std::get<int64_t>(v), p.ival);
          else
            ok = ApplyCompareOp(p.op, std::get<uint64_t>(v), p.uval);
        }
      } else {
        const char* ve = base + null_bitmap_bytes_;
        uint32_t end;
        std::memcpy(&end, ve + p.slot * sizeof(uint32_t), sizeof(uint32_t));
        uint32_t start = 0;
        if (p.slot > 0)
          std::memcpy(&start, ve + (p.slot - 1) * sizeof(uint32_t),
                      sizeof(uint32_t));
        std::string_view attr(base + unordered_base_ + start, end - start);
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
      if (cond.attr_idx >= options.attr_specs.size())
        return rocksdb::Status::InvalidArgument(
            "attr_idx " + std::to_string(cond.attr_idx) +
            " out of range (attr_num=" +
            std::to_string(options.attr_specs.size()) + ")");
      const AttrSpec& spec = options.attr_specs[cond.attr_idx];
      AttrRole type = spec.role;
      if (type == AttrRole::ORDERED) {
        bool type_ok =
            spec.is_float    ? std::holds_alternative<double>(cond.value)
            : spec.is_signed ? std::holds_alternative<int64_t>(cond.value)
                             : std::holds_alternative<uint64_t>(cond.value);
        if (!type_ok)
          return rocksdb::Status::InvalidArgument(
              "attr " + std::to_string(cond.attr_idx) +
              " ORDERED comparand type does not match its physical spec");
      } else if (type == AttrRole::UNORDERED) {
        if (!std::holds_alternative<std::string>(cond.value))
          return rocksdb::Status::InvalidArgument(
              "attr " + std::to_string(cond.attr_idx) +
              " is UNORDERED but value is not string");
        if (cond.op != CompareOp::EQUAL)
          return rocksdb::Status::InvalidArgument(
              "unordered attr " + std::to_string(cond.attr_idx) +
              " supports only EQUAL");
      }
    }
  }
  return rocksdb::Status::OK();
}

SABIQuery EncodeQuery(const BitLSMQuery& q, const BitLSMOptions& options) {
  SABIQuery out;
  out.clause_groups.reserve(q.clause_groups.size());
  for (const auto& clause : q.clause_groups) {
    SABIOrClause enc;
    enc.reserve(clause.size());
    for (const auto& c : clause) {
      SABICondition sc;
      sc.attr_idx = c.attr_idx;
      sc.op = c.op;
      if (options.attr_specs[c.attr_idx].role == AttrRole::ORDERED) {
        sc.okey = OrderedToOkey(c.value);
      } else {
        sc.bytes = std::get<std::string>(c.value);
      }
      enc.push_back(std::move(sc));
    }
    out.clause_groups.push_back(std::move(enc));
  }
  return out;
}
}  // namespace bit_lsm