#include <gtest/gtest.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <stdexcept>

#include "bit_lsm.h"
#include "bit_lsm_option.h"

// gtest builds and the bitlsm headers are on the include path.
TEST(Smoke, BitLSMOptionsConstructs) {
  bit_lsm::BitLSMOptions options;
  options.attr_num = 1;
  options.attr_specs = {bit_lsm::AttrSpec(bit_lsm::IndexType::kRange,
                                          bit_lsm::PhysicalType::kFloat, 8)};
  options.read_seqno = 0;
  options.rho = 0.5;
  EXPECT_EQ(options.attr_num, 1u);
  EXPECT_EQ(options.attr_specs.size(), 1u);
}

// The library links: the BitLSM constructor throws when opening a missing
// path with create_if_missing=false (the default).
TEST(Smoke, BitLSMConstructorLinksAndThrowsOnBadOpen) {
  bit_lsm::BitLSMOptions options;
  options.attr_num = 1;
  options.attr_specs = {bit_lsm::AttrSpec(bit_lsm::IndexType::kRange,
                                          bit_lsm::PhysicalType::kFloat, 8)};
  options.read_seqno = 0;
  options.rho = 0.5;
  EXPECT_THROW(
      bit_lsm::BitLSM("/nonexistent/bitlsm_smoke_dir", options,
                      rocksdb::Options(), rocksdb::BlockBasedTableOptions()),
      std::runtime_error);
}
