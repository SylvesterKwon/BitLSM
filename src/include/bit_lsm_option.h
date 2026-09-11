#pragma once

#include <cstdint>
#include <vector>

#include "rocksdb/types.h"

namespace bit_lsm {

// Which predicates the index accelerates on an attribute. kRange bins by
// equal-mass boundaries and serves =, <, <=, >, >=; kEquality bins by a
// frequency-balanced value dictionary and serves = only. Persisted as a byte
// in the SABI directory: never renumber.
enum class IndexType : uint8_t { kEquality = 0, kRange = 1 };

// Full spec for one attribute. The physical fields (width/is_signed/is_float)
// apply only to kRange attributes, which store a fixed-width native value;
// kEquality attributes are variable-width opaque bytes and ignore them.
// Constructing from a bare IndexType is explicit; the field defaults describe a
// double-valued, non-nullable kRange attribute (the pre-v3 physical shape).
struct AttrSpec {
  IndexType index_type;
  uint8_t width;   // kRange byte width: 1/2/4/8
  bool is_signed;  // kRange integer signedness (ignored when is_float)
  bool is_float;   // kRange: IEEE754 (true) vs integer (false)
  bool nullable;

  explicit AttrSpec(IndexType index_type = IndexType::kRange, uint8_t width = 8,
                    bool is_signed = true, bool is_float = true,
                    bool nullable = false)
      : index_type(index_type),
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
  // own per-file cache key. Off by default: the resident path stays the
  // fastest while the index fits in memory. On-demand bin reads bypass block
  // checksums -- a corrupted bin aborts (CRoaring frozen-view validation)
  // rather than returning a Status.
  bool ondemand_index = false;
};
}  // namespace bit_lsm