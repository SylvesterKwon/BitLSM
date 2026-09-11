#pragma once

// Order-preserving uint64 ("okey") domain for SABI: the adapter encodes each
// kRange native scalar through a monotone injection so the core orders
// attributes with a single unsigned comparison; kEquality attrs stay opaque
// bytes.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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

// ---- okey <-> SABI byte domain ----
// SABI orders every range-indexed attribute by memcmp over bytes. An okey
// enters that domain as its 8-byte big-endian form, which memcmp orders
// exactly like the unsigned integer.
inline constexpr size_t kOkeyBytes = 8;

inline void OkeyToBytes(uint64_t okey, char* out) {
  for (int i = static_cast<int>(kOkeyBytes) - 1; i >= 0; --i) {
    out[i] = static_cast<char>(okey & 0xff);
    okey >>= 8;
  }
}

inline std::string OkeyToBytes(uint64_t okey) {
  std::string s(kOkeyBytes, '\0');
  OkeyToBytes(okey, s.data());
  return s;
}

// Precondition: bytes.size() == kOkeyBytes.
inline uint64_t OkeyFromBytes(std::string_view bytes) {
  uint64_t okey = 0;
  for (unsigned char c : bytes) okey = (okey << 8) | c;
  return okey;
}

// ---- dispatch helpers (adapter side; the only spec-aware entry points) ----

// Decoded row scalar (AttrView from DecodeAttr) -> okey. Caller guarantees a
// non-NULL kRange input.
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
// ---- The complete schema residue visible to SABI ----
// Width/signedness/collation are absorbed by the adapter; NULL arrives as a
// per-row monostate from the extractor, never as a static flag.
struct SABISchema {
  std::vector<IndexType> index_types;
  double rho = 0.001;  // bitmap budget knob; only the builder consumes it

  uint32_t attr_num() const { return static_cast<uint32_t>(index_types.size()); }

  static SABISchema FromOptions(const BitLSMOptions& o) {
    SABISchema s;
    s.index_types.reserve(o.attr_num);
    for (const auto& sp : o.attr_specs) s.index_types.push_back(sp.index_type);
    s.rho = o.rho;
    return s;
  }
};

// ---- Row -> encoded attrs bridge ----

// Per-attr extraction result handed to SABI: SQL NULL, or the attr's bytes
// in SABI's memcmp domain -- an kRange numeric's okey in 8-byte big-endian
// form, an kEquality attr's raw bytes. Views are valid only during the
// ExtractAll call that produced them.
using EncodedAttr = std::variant<std::monostate, std::string_view>;

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

// Default extractor over the BitLSM v3 row value format. Numeric attrs are
// re-encoded into member scratch (8 bytes per attr), so the views handed out
// stay valid for the whole ExtractAll call.
class ValueLayoutExtractor : public AttrExtractor {
 public:
  explicit ValueLayoutExtractor(const BitLSMOptions& options)
      : layout_(options), scratch_(options.attr_num * kOkeyBytes, '\0') {}

  void ExtractAll(std::string_view /*key*/, std::string_view row_value,
                  EncodedAttr* out) override {
    for (uint32_t i = 0; i < static_cast<uint32_t>(layout_.slot.size()); ++i) {
      AttrView v = DecodeAttr(layout_, row_value, i);
      if (std::holds_alternative<std::monostate>(v)) {
        out[i] = std::monostate{};
      } else if (std::holds_alternative<std::string_view>(v)) {
        out[i] = std::get<std::string_view>(v);
      } else {
        char* p = scratch_.data() + i * kOkeyBytes;
        OkeyToBytes(OrderedToOkey(v), p);
        out[i] = std::string_view(p, kOkeyBytes);
      }
    }
  }

 private:
  ValueLayout layout_;
  std::string scratch_;
};

}  // namespace bit_lsm
