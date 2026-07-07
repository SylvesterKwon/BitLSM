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

using Attr = std::variant<double, std::string>;

namespace bit_lsm {

// Value format v2 (versioned via kBitLSMFormatVersion in sabi.h):
//   [var_end u32 x n_unordered][cont values 8B x n_cont][cat bytes][payload]
// Only information that cannot be derived from the schema is stored:
// var_end[r] is the end offset of the r-th unordered attr's bytes relative
// to unordered_base. Fixed-width (ordered) attrs live at schema-derived
// offsets, and the payload spans [unordered_base + var_end[n_unordered-1],
// value.size()), so neither needs a stored offset.
struct ValueLayout {
  uint32_t n_unordered = 0;
  uint32_t unordered_base = 0;
  // attr_idx -> ORDERED: absolute byte offset of the 8B double
  //             UNORDERED: rank among unordered attrs
  std::vector<uint32_t> slot;
  std::vector<uint8_t> is_ordered;

  explicit ValueLayout(const BitLSMOptions& options) {
    uint32_t attr_num = options.attr_num;
    slot.resize(attr_num);
    is_ordered.resize(attr_num);
    for (uint32_t i = 0; i < attr_num; ++i)
      if (options.attr_specs[i].role == AttrRole::UNORDERED) n_unordered++;

    uint32_t var_table = n_unordered * static_cast<uint32_t>(sizeof(uint32_t));
    uint32_t ordered_seen = 0, unordered_seen = 0;
    for (uint32_t i = 0; i < attr_num; ++i) {
      if (options.attr_specs[i].role == AttrRole::ORDERED) {
        is_ordered[i] = 1;
        slot[i] =
            var_table + ordered_seen * static_cast<uint32_t>(sizeof(double));
        ordered_seen++;
      } else {
        is_ordered[i] = 0;
        slot[i] = unordered_seen++;
      }
    }
    unordered_base =
        var_table + ordered_seen * static_cast<uint32_t>(sizeof(double));
  }
};

// Encode given attributes and payload into the v2 value format
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
      double val = std::get<double>(attrs[i]);
      std::memcpy(base + layout.slot[i], &val, sizeof(double));
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

using AttrView = std::variant<double, std::string_view>;

// Decode
inline AttrView DecodeAttr(const ValueLayout& layout, std::string_view buffer,
                           uint32_t attr_idx) {
  const char* base = buffer.data();
  if (layout.is_ordered[attr_idx]) {
    double val;
    std::memcpy(&val, base + layout.slot[attr_idx], sizeof(double));
    return val;
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
