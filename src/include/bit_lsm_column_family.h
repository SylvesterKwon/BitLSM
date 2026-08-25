#pragma once

#include <rocksdb/db.h>

#include <memory>
#include <string>

#include "bit_lsm_estimator.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"  // ValueLayout

namespace bit_lsm {

class BitLSM;

// One column family to open: its name and its schema/knobs. Mirrors
// rocksdb::ColumnFamilyDescriptor; the rocksdb-side ColumnFamilyOptions
// (per-CF SABI factory included) are derived internally by BitLSM.
struct ColumnFamilyDescriptor {
  std::string name;
  BitLSMOptions options;
};

// Per-CF facade, created and owned by BitLSM. Holds everything schema-bound
// so per-op work needs no registry lookup. Valid until DropColumnFamily()
// or BitLSM destruction (rocksdb handle-lifetime convention: outstanding
// iterators/ops on a dropped handle are caller error).
class ColumnFamilyHandle {
 public:
  // Address-identity handle: BitLSM stores it behind unique_ptr and hands
  // out raw pointers, so it must never be copied or moved.
  ColumnFamilyHandle(const ColumnFamilyHandle&) = delete;
  ColumnFamilyHandle& operator=(const ColumnFamilyHandle&) = delete;

  const std::string& name() const { return name_; }
  uint32_t id() const { return rocksdb_handle_->GetID(); }
  const BitLSMOptions& options() const { return options_; }
  const ValueLayout& layout() const { return layout_; }
  rocksdb::ColumnFamilyHandle* rocksdb_handle() const {
    return rocksdb_handle_;
  }
  // nullptr unless this CF's BitLSMOptions::enable_estimator is set.
  CardinalityEstimator* Estimator() const { return estimator_.get(); }

 private:
  friend class BitLSM;
  ColumnFamilyHandle(std::string name, rocksdb::ColumnFamilyHandle* handle,
                     const BitLSMOptions& options)
      : name_(std::move(name)),
        rocksdb_handle_(handle),
        options_(options),
        layout_(options) {}

  std::string name_;
  rocksdb::ColumnFamilyHandle* rocksdb_handle_;  // owned by BitLSM
  BitLSMOptions options_;
  ValueLayout layout_;
  std::unique_ptr<CardinalityEstimator> estimator_;
};

}  // namespace bit_lsm
