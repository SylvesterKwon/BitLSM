#pragma once

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bit_lsm_option.h"

// A row attribute value. ORDERED attrs carry a native scalar (int64/uint64 per
// signedness, or double for float/double); UNORDERED attrs carry opaque bytes.
using Attr = std::variant<int64_t, uint64_t, double, std::string>;

namespace bit_lsm {

// Value format v3 (versioned via kBitLSMFormatVersion in sabi.h):
//   [var_end u32 x n_unordered][ordered native bytes][unordered bytes][payload]
// Only information that cannot be derived from the schema is stored: var_end[r]
// is the end offset of the r-th unordered attr's bytes relative to
// unordered_base. Fixed-width ORDERED attrs live at schema-derived offsets
// (each spec.width bytes; double = 8B, so a double-only schema is
// byte-identical to v2), and the payload spans [unordered_base + var_end[last],
// value.size()), so neither needs a stored offset.
struct ValueLayout {
  uint32_t n_unordered = 0;
  uint32_t unordered_base = 0;
  // attr_idx -> ORDERED: absolute byte offset of the native value
  //             UNORDERED: rank among unordered attrs
  std::vector<uint32_t> slot;
  std::vector<uint8_t> is_ordered;
  std::vector<AttrSpec> specs;  // per-attr physical spec (width/signed/float)

  explicit ValueLayout(const BitLSMOptions& options)
      : specs(options.attr_specs) {
    uint32_t attr_num = options.attr_num;
    slot.resize(attr_num);
    is_ordered.resize(attr_num);
    for (uint32_t i = 0; i < attr_num; ++i)
      if (specs[i].role == AttrRole::UNORDERED) n_unordered++;

    uint32_t off = n_unordered * static_cast<uint32_t>(sizeof(uint32_t));
    uint32_t unordered_rank = 0;
    for (uint32_t i = 0; i < attr_num; ++i) {
      if (specs[i].role == AttrRole::ORDERED) {
        is_ordered[i] = 1;
        slot[i] = off;
        off += specs[i].width;
      } else {
        is_ordered[i] = 0;
        slot[i] = unordered_rank++;
      }
    }
    unordered_base = off;
  }
};

using AttrView = std::variant<int64_t, uint64_t, double, std::string_view>;

// Write an ORDERED attr's native value as spec.width little-endian bytes.
inline void EncodeOrdered(char* dst, const Attr& v, const AttrSpec& s) {
  if (s.is_float) {
    double d = std::get<double>(v);
    if (s.width == 8) {
      std::memcpy(dst, &d, 8);
    } else {  // width == 4: single-precision float
      float f = static_cast<float>(d);
      std::memcpy(dst, &f, 4);
    }
  } else if (s.is_signed) {
    int64_t x = std::get<int64_t>(v);
    std::memcpy(dst, &x, s.width);  // low width bytes (two's complement, LE)
  } else {
    uint64_t x = std::get<uint64_t>(v);
    std::memcpy(dst, &x, s.width);
  }
}

// Read an ORDERED attr's native value (spec.width bytes) back to a widened
// AttrView (int64/uint64/double).
inline AttrView DecodeOrdered(const char* src, const AttrSpec& s) {
  if (s.is_float) {
    if (s.width == 8) {
      double d;
      std::memcpy(&d, src, 8);
      return d;
    }
    float f;
    std::memcpy(&f, src, 4);
    return static_cast<double>(f);
  }
  uint64_t raw = 0;
  std::memcpy(&raw, src, s.width);
  if (s.is_signed) {
    if (s.width < 8) {  // sign-extend from width*8 bits
      uint64_t sign_bit = 1ull << (s.width * 8 - 1);
      if (raw & sign_bit) raw |= ~((1ull << (s.width * 8)) - 1);
    }
    int64_t x;
    std::memcpy(&x, &raw, 8);
    return x;
  }
  return raw;
}

// Encode given attributes and payload into the v3 value format
inline void EncodeValue(const ValueLayout& layout,
                        const std::vector<Attr>& attrs,
                        std::string_view payload, std::string& out_value) {
  uint32_t unordered_bytes = 0;
  for (size_t i = 0; i < attrs.size(); ++i) {
    if (!layout.is_ordered[i])
      unordered_bytes +=
          static_cast<uint32_t>(std::get<std::string>(attrs[i]).size());
  }

  out_value.resize(layout.unordered_base + unordered_bytes + payload.size());
  char* base = out_value.data();
  char* unordered_ptr = base + layout.unordered_base;
  uint32_t var_end = 0;
  uint32_t unordered_seen = 0;
  for (size_t i = 0; i < attrs.size(); ++i) {
    if (layout.is_ordered[i]) {
      EncodeOrdered(base + layout.slot[i], attrs[i], layout.specs[i]);
    } else {
      const std::string& str = std::get<std::string>(attrs[i]);
      std::memcpy(unordered_ptr, str.data(), str.size());
      unordered_ptr += str.size();
      var_end += static_cast<uint32_t>(str.size());
      std::memcpy(base + unordered_seen * sizeof(uint32_t), &var_end,
                  sizeof(uint32_t));
      unordered_seen++;
    }
  }
  if (!payload.empty())
    std::memcpy(unordered_ptr, payload.data(), payload.size());
}

// Convenience overload for callers without a cached layout (tests, tools)
inline void EncodeValue(const BitLSMOptions& options,
                        const std::vector<Attr>& attrs,
                        std::string_view payload, std::string& out_value) {
  EncodeValue(ValueLayout(options), attrs, payload, out_value);
}

// Decode
inline AttrView DecodeAttr(const ValueLayout& layout, std::string_view buffer,
                           uint32_t attr_idx) {
  const char* base = buffer.data();
  if (layout.is_ordered[attr_idx]) {
    return DecodeOrdered(base + layout.slot[attr_idx], layout.specs[attr_idx]);
  }
  uint32_t rank = layout.slot[attr_idx];
  uint32_t end;
  std::memcpy(&end, base + rank * sizeof(uint32_t), sizeof(uint32_t));
  uint32_t start = 0;
  if (rank > 0)
    std::memcpy(&start, base + (rank - 1) * sizeof(uint32_t), sizeof(uint32_t));
  return std::string_view(base + layout.unordered_base + start, end - start);
}

// Convenience overload for callers without a cached layout (tests, tools)
inline AttrView DecodeAttr(const BitLSMOptions& options,
                           std::string_view buffer, uint32_t attr_idx) {
  return DecodeAttr(ValueLayout(options), buffer, attr_idx);
}

// Payload spans from the last unordered end to the end of the value
inline std::string_view DecodePayload(const ValueLayout& layout,
                                      std::string_view buffer) {
  uint32_t start = 0;
  if (layout.n_unordered > 0)
    std::memcpy(&start,
                buffer.data() + (layout.n_unordered - 1) * sizeof(uint32_t),
                sizeof(uint32_t));
  return buffer.substr(layout.unordered_base + start);
}

inline void TEST_DumpValue(BitLSMOptions options, rocksdb::Slice input) {
  ValueLayout layout(options);
  for (uint32_t i = 0; i < options.attr_num; ++i) {
    if (i) std::cout << " / ";
    AttrView av = DecodeAttr(layout, input.ToStringView(), i);
    if (options.attr_specs[i].role == AttrRole::ORDERED) {
      std::cout << std::fixed << std::setprecision(6) << std::get<double>(av);
    } else {
      std::cout << std::get<std::string_view>(av);
    }
  }
  std::cout << "\n";
}

}  // namespace bit_lsm
