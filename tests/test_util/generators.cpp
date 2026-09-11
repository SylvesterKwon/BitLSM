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

// A fresh value for `spec`: numerics within the width's range (floats
// rounded through the stored precision so the oracle holds what the engine
// holds), binaries from the dictionary, padded to a fixed width when needed.
Attr RandomValue(Rng& rng, const AttrSpec& spec, const WorkloadParams& p) {
  switch (spec.physical_type) {
    case PhysicalType::kInt: {
      const int64_t lim = spec.width == 1 ? 100 : 1000;
      return std::uniform_int_distribution<int64_t>(-lim, lim)(rng);
    }
    case PhysicalType::kUint:
      return static_cast<uint64_t>(
          std::uniform_int_distribution<uint32_t>(0, 200)(rng));
    case PhysicalType::kFloat: {
      const double d = UniformValue(rng);
      return spec.width == 4 ? static_cast<double>(static_cast<float>(d)) : d;
    }
    case PhysicalType::kBinary: {
      std::string v = DictValue(rng, p.unordered_dict);
      v.resize(spec.width, ' ');
      return v;
    }
    case PhysicalType::kVarBinary:
      return DictValue(rng, p.unordered_dict);
  }
  return std::monostate{};
}

// `stored` nudged one step in its own domain (bin-boundary pressure); binary
// values have no natural neighbour and come back unchanged. A float nudge is
// rounded through the stored precision so the oracle holds what the engine
// decodes.
Attr Nearby(Rng& rng, const AttrSpec& spec, const Attr& stored) {
  if (std::holds_alternative<int64_t>(stored))
    return std::get<int64_t>(stored) + (rng() % 2 == 0 ? 1 : -1);
  if (std::holds_alternative<uint64_t>(stored))
    return std::get<uint64_t>(stored) + 1;
  if (std::holds_alternative<double>(stored)) {
    const double d = std::get<double>(stored) +
                     std::uniform_real_distribution<double>(-0.5, 0.5)(rng);
    return spec.width == 4 ? static_cast<double>(static_cast<float>(d)) : d;
  }
  return stored;
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
  o.attr_specs.reserve(o.attr_num);
  static constexpr uint16_t kIntWidths[] = {1, 2, 4, 8};
  static constexpr uint16_t kFloatWidths[] = {4, 8};
  static constexpr uint16_t kBinaryWidths[] = {4, 8, 16};
  for (uint32_t i = 0; i < o.attr_num; ++i) {
    const IndexType it =
        rng() % 2 == 0 ? IndexType::kRange : IndexType::kEquality;
    const PhysicalType pt = static_cast<PhysicalType>(rng() % 5);
    uint16_t width = 0;
    switch (pt) {
      case PhysicalType::kInt:
      case PhysicalType::kUint:
        width = kIntWidths[rng() % 4];
        break;
      case PhysicalType::kFloat:
        width = kFloatWidths[rng() % 2];
        break;
      case PhysicalType::kBinary:
        width = kBinaryWidths[rng() % 3];
        break;
      case PhysicalType::kVarBinary:
        break;
    }
    o.attr_specs.emplace_back(it, pt, width);
  }
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
    const AttrSpec& spec = schema.attr_specs[i];
    const uint32_t mode = rng() % 4;
    Attr stored;
    if (mode == 0 && SampleStored(rng, oracle, i, &stored) &&
        !std::holds_alternative<std::monostate>(stored)) {
      attrs[i] = stored;  // exact repeat
    } else if (mode == 1 && spec.IsNumeric() &&
               SampleStored(rng, oracle, i, &stored) &&
               !std::holds_alternative<std::monostate>(stored)) {
      attrs[i] = Nearby(rng, spec, stored);
    } else {
      attrs[i] = RandomValue(rng, spec, p);
    }
  }
  return attrs;
}

BitLSMQuery GenerateQuery(Rng& rng, const BitLSMOptions& schema,
                          const WorkloadParams& p, const ReferenceDB& oracle) {
  uint32_t num_clauses = std::uniform_int_distribution<uint32_t>(0, 3)(rng);
  std::vector<OrClause> clauses;
  clauses.reserve(num_clauses);
  static constexpr CompareOp kOps[] = {
      CompareOp::EQUAL, CompareOp::LESS, CompareOp::LESS_EQUAL,
      CompareOp::GREATER, CompareOp::GREATER_EQUAL};
  for (uint32_t c = 0; c < num_clauses; ++c) {
    uint32_t num_conds = std::uniform_int_distribution<uint32_t>(1, 3)(rng);
    OrClause clause;
    for (uint32_t k = 0; k < num_conds; ++k) {
      QueryCondition cond;
      cond.attr_idx =
          std::uniform_int_distribution<uint32_t>(0, schema.attr_num - 1)(rng);
      const AttrSpec& spec = schema.attr_specs[cond.attr_idx];
      // Contract: kEquality attrs take EQUAL only.
      cond.op = spec.index_type == IndexType::kEquality ? CompareOp::EQUAL
                                                        : kOps[rng() % 5];
      Attr v;
      Attr stored;
      const uint32_t mode = rng() % 4;
      if (mode <= 1 && SampleStored(rng, oracle, cond.attr_idx, &stored) &&
          !std::holds_alternative<std::monostate>(stored)) {
        v = stored;  // stored value: EQUAL hits, boundaries for range ops
      } else if (mode == 2 && spec.IsNumeric() &&
                 SampleStored(rng, oracle, cond.attr_idx, &stored) &&
                 !std::holds_alternative<std::monostate>(stored)) {
        v = Nearby(rng, spec, stored);
      } else {
        v = RandomValue(rng, spec, p);
      }
      if (std::holds_alternative<int64_t>(v))
        cond.value = std::get<int64_t>(v);
      else if (std::holds_alternative<uint64_t>(v))
        cond.value = std::get<uint64_t>(v);
      else if (std::holds_alternative<double>(v))
        cond.value = std::get<double>(v);
      else
        cond.value = std::get<std::string>(v);
      clause.push_back(std::move(cond));
    }
    clauses.push_back(std::move(clause));
  }
  return BitLSMQuery(std::move(clauses));
}

}  // namespace bit_lsm
