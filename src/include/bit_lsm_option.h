#pragma once

#include <cstdint>
#include <vector>

#include "rocksdb/types.h"

namespace bit_lsm {

// Attribute role: ORDERED = total order (range queries + quantile binning),
// UNORDERED = equality only (frequency binning).
enum AttrRole {
  UNORDERED,
  ORDERED,
};

// Full spec for one attribute. The physical fields (width/is_signed/is_float)
// apply only to ORDERED attributes, which store a fixed-width native value;
// UNORDERED attributes are variable-width opaque bytes and ignore them.
// Implicitly constructible from a bare AttrRole role, defaulting to a
// double-valued ORDERED attribute — i.e. the pre-v3 behavior.
struct AttrSpec {
  AttrRole role;
  uint8_t width;   // ORDERED byte width: 1/2/4/8
  bool is_signed;  // ORDERED integer signedness (ignored when is_float)
  bool is_float;   // ORDERED: IEEE754 (true) vs integer (false)
  bool nullable;

  AttrSpec(AttrRole role = ORDERED, uint8_t width = 8, bool is_signed = true,
           bool is_float = true, bool nullable = false)
      : role(role),
        width(width),
        is_signed(is_signed),
        is_float(is_float),
        nullable(nullable) {}

  bool operator==(const AttrSpec&) const = default;
};

struct BitLSMOptions {
  uint32_t attr_num;                   // # of attribute
  std::vector<AttrSpec> attr_specs;    // per-attribute spec
  rocksdb::SequenceNumber read_seqno;  // read sequence number
  double rho;  // proportion parameter that determines bitmap budget
};
}  // namespace bit_lsm