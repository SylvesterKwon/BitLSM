#include <gtest/gtest.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <stdexcept>

#include "bit_lsm.h"
#include "bit_lsm_option.h"

// 목적: gtest 빌드 + bitlsm 헤더 인클루드 경로 확인.
TEST(Smoke, BitLSMOptionsConstructs) {
  bit_lsm::BitLSMOptions options;
  options.attr_num = 1;
  options.attr_specs = {bit_lsm::AttrSpec{bit_lsm::AttrRole::ORDERED}};
  options.read_seqno = 0;
  options.rho = 0.5;
  EXPECT_EQ(options.attr_num, 1u);
  EXPECT_EQ(options.attr_specs.size(), 1u);
}

// 목적: 컴파일된 라이브러리 심볼(BitLSM 생성자, bit_lsm.cpp)이 실제로
// 링크되는지 확인. create_if_missing=false(기본값)로 없는 경로를 열면 생성자가
// throw 한다.
TEST(Smoke, BitLSMConstructorLinksAndThrowsOnBadOpen) {
  bit_lsm::BitLSMOptions options;
  options.attr_num = 1;
  options.attr_specs = {bit_lsm::AttrSpec{bit_lsm::AttrRole::ORDERED}};
  options.read_seqno = 0;
  options.rho = 0.5;
  EXPECT_THROW(
      bit_lsm::BitLSM("/nonexistent/bitlsm_smoke_dir", options,
                      rocksdb::Options(), rocksdb::BlockBasedTableOptions()),
      std::runtime_error);
}
