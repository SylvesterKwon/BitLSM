#pragma once

// Order-preserving uint64 ("okey") domain for SABI: the adapter encodes each
// ORDERED native scalar through a monotone injection so the core orders
// attributes with a single unsigned comparison; UNORDERED attrs stay opaque
// bytes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"

namespace bit_lsm {

// ---- native -> okey (monotone bijections, width <= 8 bytes lossless) ----

inline uint64_t U64ToOkey(uint64_t x) { return x; }

inline uint64_t I64ToOkey(int64_t x) {
  // Flip the sign bit: MIN -> 0x00.., -1 -> 0x7F.., 0 -> 0x80.., MAX -> 0xFF..
  return static_cast<uint64_t>(x) ^ 0x8000000000000000ull;
}

inline uint64_t F64ToOkey(double d) {
  // IEEE-754 total-order trick: positives flip the sign bit, negatives flip
  // all bits. -0.0 is canonicalized to +0.0 first: native == treats them as
  // equal, so distinct okeys could prune a -0.0 row on an EQ +0.0 query.
  if (d == 0.0) d = 0.0;
  uint64_t u;
  std::memcpy(&u, &d, 8);
  return (u & 0x8000000000000000ull) ? ~u : (u ^ 0x8000000000000000ull);
}

// ---- okey -> native (debug/Dump only; not used on any query path) ----

inline int64_t OkeyToI64(uint64_t okey) {
  return static_cast<int64_t>(okey ^ 0x8000000000000000ull);
}

inline double OkeyToF64(uint64_t okey) {
  uint64_t u =
      (okey & 0x8000000000000000ull) ? (okey ^ 0x8000000000000000ull) : ~okey;
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

// ---- dispatch helpers (adapter side; the only spec-aware entry points) ----

// Decoded row scalar (AttrView from DecodeAttr) -> okey. Caller guarantees a
// non-NULL ORDERED input.
inline uint64_t OrderedToOkey(const AttrView& v) {
  if (std::holds_alternative<int64_t>(v))
    return I64ToOkey(std::get<int64_t>(v));
  if (std::holds_alternative<uint64_t>(v))
    return U64ToOkey(std::get<uint64_t>(v));
  return F64ToOkey(std::get<double>(v));
}

// Query comparand -> okey (string alternative unreachable past Validate()).
inline uint64_t OrderedToOkey(
    const std::variant<int64_t, uint64_t, double, std::string>& v) {
  if (std::holds_alternative<int64_t>(v))
    return I64ToOkey(std::get<int64_t>(v));
  if (std::holds_alternative<uint64_t>(v))
    return U64ToOkey(std::get<uint64_t>(v));
  return F64ToOkey(std::get<double>(v));
}

// ---- t-digest bridge ----

// okey -> t-digest double domain, shifted by the per-SST minimum so the
// 52-bit mantissa covers the okey span instead of the absolute magnitude.
// Unshifted, the double ULP near 2^63 (where every small signed int lands)
// is 2048: an attribute whose per-SST span is narrower collapses onto one
// representable double, folding every row into a single bin. The shift is
// exact whenever span < 2^53; above that it degrades to interior-boundary
// rounding only, never bin membership: all membership decisions compare
// okeys, never doubles.
inline double OkeyToTDigest(uint64_t okey, uint64_t min_okey) {
  return static_cast<double>(okey - min_okey);
}

// t-digest quantile estimate (shifted double) -> absolute okey threshold.
// Any monotone, clamped rounding is valid: boundaries are arbitrary
// thresholds as long as build-time bin assignment and query-time bin mapping
// share them.
inline uint64_t TDigestBoundaryToOkey(double d, uint64_t min_okey) {
  if (std::isnan(d) || d <= 0.0) return min_okey;
  if (d >= static_cast<double>(UINT64_MAX - min_okey)) return UINT64_MAX;
  return min_okey + static_cast<uint64_t>(d);
}

// ---- Level-aware rho (gamma decay) ----

// RocksDB's default; BitLSM never overrides Options::num_levels, so the LSM
// spans L0..L6.
inline constexpr int kDefaultNumLevels = 7;

// d = distance from the deepest level. Unknown level (-1, e.g.
// SstFileWriter-driven builds) falls back to the maximum distance = maximum
// decay = minimum budget.
inline uint32_t LevelDistanceFromDeepest(int level_at_creation,
                                         int num_levels) {
  if (num_levels <= 0) num_levels = kDefaultNumLevels;
  if (level_at_creation < 0 || level_at_creation >= num_levels) {
    return static_cast<uint32_t>(num_levels - 1);
  }
  return static_cast<uint32_t>((num_levels - 1) - level_at_creation);
}

// gamma == 1.0 short-circuits to plain rho: the exact same double flows into
// the bin-budget division (byte-identical blobs), and the 0.5 clamp — which
// would alter behavior for rho > 0.5 — is never applied.
inline double EffectiveRho(double rho, double gamma, uint32_t d) {
  if (gamma == 1.0) return rho;
  return std::min(rho * std::pow(gamma, static_cast<double>(d)), 0.5);
}

// ---- The complete schema residue visible to SABI ----
// Width/signedness/collation are absorbed by the adapter; NULL arrives as a
// per-row monostate from the extractor, never as a static flag.
struct SABISchema {
  std::vector<AttrRole> roles;
  double rho = 0.01;   // bitmap budget knob; only the builder consumes it
  double gamma = 1.0;  // level-aware rho decay constant; 1.0 = off

  uint32_t attr_num() const { return static_cast<uint32_t>(roles.size()); }

  static SABISchema FromOptions(const BitLSMOptions& o) {
    SABISchema s;
    s.roles.reserve(o.attr_num);
    for (const auto& sp : o.attr_specs) s.roles.push_back(sp.role);
    s.rho = o.rho;
    s.gamma = o.gamma;
    return s;
  }
};

// ---- Row -> encoded attrs bridge ----

// Per-attr extraction result handed to SABI: SQL NULL, an ORDERED okey, or
// UNORDERED opaque bytes (a view into the row value; valid only during the
// ExtractAll call that produced it).
using EncodedAttr = std::variant<std::monostate, uint64_t, std::string_view>;

// SABI's only path from a row to attrs: one virtual ExtractAll call per row;
// the implementation owns all remaining schema knowledge (layout, widths,
// collation).
//
// Each builder owns its extractor exclusively (SABIFactory creates one per
// NewBuilder), so implementations may keep plain member scratch.
//
// `key` lets an implementation recognize rows of a foreign table/index
// sharing the CF; convention for those: fill `out` with all-monostate so the
// row lands in no value bin.
class AttrExtractor {
 public:
  virtual ~AttrExtractor() = default;
  // `out` must have schema.attr_num() slots, preallocated by the caller.
  virtual void ExtractAll(std::string_view key, std::string_view row_value,
                          EncodedAttr* out) = 0;
};

// Default extractor over the BitLSM v3 row value format.
class ValueLayoutExtractor : public AttrExtractor {
 public:
  explicit ValueLayoutExtractor(const BitLSMOptions& options)
      : layout_(options) {}

  void ExtractAll(std::string_view /*key*/, std::string_view row_value,
                  EncodedAttr* out) override {
    for (uint32_t i = 0; i < static_cast<uint32_t>(layout_.slot.size()); ++i) {
      AttrView v = DecodeAttr(layout_, row_value, i);
      if (std::holds_alternative<std::monostate>(v)) {
        out[i] = std::monostate{};
      } else if (layout_.is_ordered[i]) {
        out[i] = OrderedToOkey(v);
      } else {
        out[i] = std::get<std::string_view>(v);
      }
    }
  }

 private:
  ValueLayout layout_;
};

}  // namespace bit_lsm
