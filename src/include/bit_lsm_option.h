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

  // Scan data-block reads kept in flight (block_prefetch_queue.h); 0 = one at a
  // time, the pre-existing behaviour. Costs one block-sized buffer per
  // outstanding read per live table iterator, so depth * block_size * levels.
  // Silently does nothing unless the build found liburing.
  uint32_t scan_prefetch_depth = 0;

  // Cardinality estimator (read-side planning stats; bit_lsm_estimator.h).
  // Off by default; none of these knobs touch the SST format.
  bool enable_estimator = false;
  uint32_t estimator_grid_cells = 256;  // per-attr okey-grid resolution
  // Floor between stats rebuilds; bounds the refresh worker's duty cycle
  // under churn.
  uint32_t estimator_min_rebuild_interval_ms = 1000;

  // Keep only the SABI directory resident per table and read bin bitmaps on
  // demand, caching the DECODED bin in the block cache keyed off RocksDB's
  // own per-file cache key. Requires format v7 blobs (opening older SSTs
  // fails with Corruption naming the rebuild). Off by default: the resident
  // path stays the fastest while the index fits in memory.
  // The version-naming Corruption lands in the RocksDB LOG; the surfaced
  // Status is a generic UDI-reader Corruption for the file. On-demand bin
  // reads bypass block checksums -- a corrupted bin aborts (CRoaring
  // frozen-view validation) rather than returning a Status.
  bool ondemand_index = false;
};
}  // namespace bit_lsm