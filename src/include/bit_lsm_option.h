#pragma once

#include <cstdint>
#include <vector>

#include "rocksdb/types.h"

namespace bit_lsm {

// Attribute role: ORDERED = total order (range queries + quantile binning),
// UNORDERED = equality only (frequency binning).
// Values are persisted as role bytes in the SABI directory (format v5+);
// never renumber existing entries.
enum AttrRole {
  UNORDERED,
  ORDERED,
};

// Full spec for one attribute. The physical fields (width/is_signed/is_float)
// apply only to ORDERED attributes, which store a fixed-width native value;
// UNORDERED attributes are variable-width opaque bytes and ignore them.
// Constructing from a bare AttrRole is explicit; the field defaults describe a
// double-valued, non-nullable ORDERED attribute (the pre-v3 physical shape).
struct AttrSpec {
  AttrRole role;
  uint8_t width;   // ORDERED byte width: 1/2/4/8
  bool is_signed;  // ORDERED integer signedness (ignored when is_float)
  bool is_float;   // ORDERED: IEEE754 (true) vs integer (false)
  bool nullable;

  explicit AttrSpec(AttrRole role = ORDERED, uint8_t width = 8,
                    bool is_signed = true, bool is_float = true,
                    bool nullable = false)
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

  // Cardinality estimator (read-side planning stats; bit_lsm_estimator.h).
  // Off by default: standalone use pays nothing unless estimation is wanted.
  // These knobs never touch the SST format.
  bool enable_estimator = false;
  uint32_t estimator_grid_cells = 256;  // per-attr okey-grid resolution (D-E2)
  // Floor between stats rebuilds: bounds the refresh worker's duty cycle
  // when churn (bulk load, compaction storms) trips the drift threshold
  // repeatedly.
  uint32_t estimator_min_rebuild_interval_ms = 1000;
};
}  // namespace bit_lsm