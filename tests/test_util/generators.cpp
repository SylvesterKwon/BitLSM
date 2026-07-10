#include "test_util/generators.h"

#include <cassert>
#include <iterator>

namespace bit_lsm {
namespace {

// Shared by stored attrs and query comparands, so both sides of an EQ can hit
// the same special value.
double UniformValue(Rng& rng) {
  // 1%: special values that hit okey encoding edges (±0.0, huge, denormal);
  // the oracle diff is the e2e correctness gate.
  if (std::uniform_int_distribution<int>(0, 99)(rng) == 0) {
    static constexpr double kSpecials[] = {-0.0,  0.0,    -1e300,
                                           1e300, 5e-324, -5e-324};
    return kSpecials[std::uniform_int_distribution<int>(0, 5)(rng)];
  }
  return std::uniform_real_distribution<double>(0.0, 100.0)(rng);
}

std::string DictValue(Rng& rng, uint32_t dict_size) {
  uint32_t v = std::uniform_int_distribution<uint32_t>(0, dict_size - 1)(rng);
  return "c" + std::to_string(v);
}

// Sample attr `idx` of a random live record (EQUAL-hit / boundary pressure).
// Returns false when the oracle is empty.
// Precondition: every oracle record carries schema.attr_num attrs.
bool SampleStored(Rng& rng, const ReferenceDB& oracle, uint32_t idx,
                  Attr* out) {
  const auto& live = oracle.live();
  if (live.empty()) return false;
  std::uint64_t skip =
      std::uniform_int_distribution<std::uint64_t>(0, live.size() - 1)(rng);
  auto it = live.begin();
  std::advance(it, skip);
  *out = it->second.attrs[idx];
  return true;
}

}  // namespace

BitLSMOptions GenerateSchema(Rng& rng) {
  BitLSMOptions o;
  o.attr_num = std::uniform_int_distribution<uint32_t>(1, 5)(rng);
  o.attr_specs.resize(o.attr_num);
  for (auto& t : o.attr_specs)
    t = (rng() % 2 == 0) ? AttrSpec{AttrRole::ORDERED}
                         : AttrSpec{AttrRole::UNORDERED};
  static constexpr double kRhos[] = {0.5, 0.2, 0.05};
  o.rho = kRhos[rng() % 3];
  o.read_seqno = 0;
  return o;
}

OpKind PickOp(Rng& rng) {
  uint32_t r = std::uniform_int_distribution<uint32_t>(0, 99)(rng);
  if (r < 60) return OpKind::kPut;
  if (r < 75) return OpKind::kDelete;
  if (r < 85) return OpKind::kPutBatch;
  if (r < 95) return OpKind::kFlush;
  return OpKind::kCompactAll;
}

std::string GenerateKey(Rng& rng, const WorkloadParams& p) {
  assert(p.key_pool > 0);
  // u^2 biases toward low ids -> hot keys collide on overwrite/delete.
  double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
  uint32_t id = static_cast<uint32_t>(u * u * p.key_pool);
  if (id >= p.key_pool) id = p.key_pool - 1;
  return "k" + std::to_string(id);
}

std::vector<Attr> GenerateAttrs(Rng& rng, const BitLSMOptions& schema,
                                const WorkloadParams& p,
                                const ReferenceDB& oracle) {
  assert(p.unordered_dict > 0);
  std::vector<Attr> attrs(schema.attr_num);
  for (uint32_t i = 0; i < schema.attr_num; ++i) {
    if (schema.attr_specs[i].role == AttrRole::UNORDERED) {
      attrs[i] = DictValue(rng, p.unordered_dict);
      continue;
    }
    uint32_t mode = rng() % 4;
    Attr stored;
    if (mode == 0 && SampleStored(rng, oracle, i, &stored) &&
        std::holds_alternative<double>(stored)) {
      attrs[i] = stored;  // exact repeat
    } else if (mode == 1 && SampleStored(rng, oracle, i, &stored) &&
               std::holds_alternative<double>(stored)) {
      attrs[i] = std::get<double>(stored) +
                 std::uniform_real_distribution<double>(-0.5, 0.5)(rng);
    } else {
      attrs[i] = UniformValue(rng);
    }
  }
  return attrs;
}

BitLSMQuery GenerateQuery(Rng& rng, const BitLSMOptions& schema,
                          const WorkloadParams& p, const ReferenceDB& oracle) {
  uint32_t num_clauses = std::uniform_int_distribution<uint32_t>(0, 3)(rng);
  std::vector<OrClause> clauses;
  clauses.reserve(num_clauses);
  for (uint32_t c = 0; c < num_clauses; ++c) {
    uint32_t num_conds = std::uniform_int_distribution<uint32_t>(1, 3)(rng);
    OrClause clause;
    for (uint32_t k = 0; k < num_conds; ++k) {
      QueryCondition cond;
      cond.attr_idx =
          std::uniform_int_distribution<uint32_t>(0, schema.attr_num - 1)(rng);
      Attr stored;
      if (schema.attr_specs[cond.attr_idx].role == AttrRole::UNORDERED) {
        cond.op = CompareOp::EQUAL;  // contract: unordered is EQUAL-only
        if (rng() % 2 == 0 &&
            SampleStored(rng, oracle, cond.attr_idx, &stored) &&
            std::holds_alternative<std::string>(stored))
          cond.value = std::get<std::string>(stored);
        else
          cond.value = DictValue(rng, p.unordered_dict);
      } else {
        static constexpr CompareOp kOps[] = {
            CompareOp::EQUAL, CompareOp::LESS, CompareOp::LESS_EQUAL,
            CompareOp::GREATER, CompareOp::GREATER_EQUAL};
        cond.op = kOps[rng() % 5];
        uint32_t mode = rng() % 4;
        if (mode <= 1 && SampleStored(rng, oracle, cond.attr_idx, &stored) &&
            std::holds_alternative<double>(stored)) {
          cond.value = std::get<double>(stored);  // stored value: EQUAL hits
        } else if (mode == 2 &&
                   SampleStored(rng, oracle, cond.attr_idx, &stored) &&
                   std::holds_alternative<double>(stored)) {
          // just off a stored value: bin-boundary pressure with range ops
          cond.value = std::get<double>(stored) +
                       std::uniform_real_distribution<double>(-0.5, 0.5)(rng);
        } else {
          cond.value = UniformValue(rng);
        }
      }
      clause.push_back(std::move(cond));
    }
    clauses.push_back(std::move(clause));
  }
  return BitLSMQuery(std::move(clauses));
}

}  // namespace bit_lsm
