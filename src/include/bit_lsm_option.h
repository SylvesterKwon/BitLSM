#pragma once

#include <cstdint>
#include <vector>

#include "rocksdb/types.h"

namespace bit_lsm {

enum AttrType {
  UNORDERED,
  ORDERED,
};

struct BitLSMOptions {
  uint32_t attr_num;                   // # of attribute
  std::vector<AttrType> attr_types;    // attribute type vector
  rocksdb::SequenceNumber read_seqno;  // read sequence number
  double rho;  // proportion parameter that determines bitmap budget
};
}  // namespace bit_lsm