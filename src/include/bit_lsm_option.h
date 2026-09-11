#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rocksdb/status.h"
#include "rocksdb/types.h"

namespace bit_lsm {

// Which predicates the index accelerates on an attribute. kRange bins by
// equal-mass boundaries and serves =, <, <=, >, >=; kEquality bins by a
// frequency-balanced value dictionary and serves = only. Persisted as a byte
// in the SABI directory: never renumber.
enum class IndexType : uint8_t { kEquality = 0, kRange = 1 };

// How an attribute's value is laid out in the row and compared. SQL naming:
// kBinary is BINARY(n) (fixed width), kVarBinary is VARBINARY (variable).
// Numeric types live in fixed slots and reach SABI as 8-byte okeys; binary
// types reach SABI as their raw bytes, ordered by memcmp.
enum class PhysicalType : uint8_t { kInt, kUint, kFloat, kBinary, kVarBinary };

// Full spec for one attribute: index type (which predicates) x physical type
// (which bytes). Every combination is valid.
struct AttrSpec {
  IndexType index_type;
  PhysicalType physical_type;
  // kInt/kUint: 1, 2, 4 or 8; kFloat: 4 or 8; kBinary: the fixed byte count;
  // kVarBinary: 0.
  uint16_t width;
  bool nullable;

  AttrSpec(IndexType index_type, PhysicalType physical_type,
           uint16_t width = 0, bool nullable = false)
      : index_type(index_type),
        physical_type(physical_type),
        width(width),
        nullable(nullable) {}

  bool IsNumeric() const {
    return physical_type == PhysicalType::kInt ||
           physical_type == PhysicalType::kUint ||
           physical_type == PhysicalType::kFloat;
  }
  bool IsFixed() const { return physical_type != PhysicalType::kVarBinary; }
  bool Valid() const {
    switch (physical_type) {
      case PhysicalType::kInt:
      case PhysicalType::kUint:
        return width == 1 || width == 2 || width == 4 || width == 8;
      case PhysicalType::kFloat:
        return width == 4 || width == 8;
      case PhysicalType::kBinary:
        return width > 0;
      case PhysicalType::kVarBinary:
        return width == 0;
    }
    return false;
  }
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
// Schema sanity for an open/create path: attr_num agrees with attr_specs and
// every spec's width fits its physical type. Returned as a status rather
// than asserted, so a Release build refuses a bad schema instead of laying
// rows out with a zero-width slot.
inline rocksdb::Status ValidateAttrSpecs(const BitLSMOptions& o) {
  if (o.attr_specs.size() != o.attr_num)
    return rocksdb::Status::InvalidArgument(
        "attr_num does not match attr_specs size");
  for (size_t i = 0; i < o.attr_specs.size(); ++i)
    if (!o.attr_specs[i].Valid())
      return rocksdb::Status::InvalidArgument(
          "attr " + std::to_string(i) + ": width " +
          std::to_string(o.attr_specs[i].width) +
          " is invalid for its physical type");
  return rocksdb::Status::OK();
}

}  // namespace bit_lsm