#pragma once

#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "bit_lsm.h"
#include "bit_lsm_option.h"

namespace bit_lsm {

// 모든 e2e 테스트의 베이스. 테스트마다 고유한 임시 DB 디렉토리를 만들고
// 끝나면 정리한다. MEM_ENV=1 이면 디스크 대신 RAM(NewMemEnv)에서 돈다.
class BitLSMTestBase : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name =
        std::string(info->test_suite_name()) + "_" + info->name();
    for (char& c : name)
      if (c == '/') c = '_';  // 파라미터화 테스트 이름의 '/' 정리

    const char* tmp_env = std::getenv("TEST_TMPDIR");
    std::string tmproot = tmp_env ? tmp_env : ::testing::TempDir();
    if (!tmproot.empty() && tmproot.back() != '/') tmproot.push_back('/');
    db_path_ = tmproot + "bitlsm_" + name + "_" + std::to_string(getpid());

    const char* mem = std::getenv("MEM_ENV");
    if (mem && std::string_view(mem) == "1") {
      mem_env_ = rocksdb::NewMemEnv(rocksdb::Env::Default());
      rocksdb_options_.env = mem_env_;
    }

    // 이전(크래시한) 런의 잔재 제거.
    rocksdb::DestroyDB(db_path_, rocksdb::Options());
    rocksdb_options_.create_if_missing = true;
  }

  void TearDown() override {
    db_.reset();  // BitLSM 소멸자가 DB 를 정상 close.
    const char* keep = std::getenv("KEEP_DB");
    if (keep && std::string_view(keep) == "1") {
      std::cerr << "KEEP_DB: leaving DB at " << db_path_ << "\n";
    } else {
      rocksdb::Options opts;
      opts.env = mem_env_ ? mem_env_ : rocksdb::Env::Default();
      rocksdb::DestroyDB(db_path_, opts);
    }
    delete mem_env_;  // 미사용 시 nullptr 이라 안전.
    mem_env_ = nullptr;
  }

  // db_path_ 에 새 BitLSM 을 연다. 소유권은 fixture.
  // 반환된 참조는 다음 OpenDB() 호출이나 TearDown 전까지만 유효하다.
  // 재호출 시 기존 DB 를 먼저 닫아 같은 db_path_ 에 대한 RocksDB LOCK 충돌을
  // 막는다(새 인스턴스가 구 인스턴스보다 먼저 생성되는 것을 방지).
  BitLSM& OpenDB(const BitLSMOptions& bitlsm_options) {
    db_.reset();  // 같은 경로를 다시 열기 전에 기존 DB 를 명시적으로 닫는다.
    db_ = std::make_unique<BitLSM>(db_path_, bitlsm_options, rocksdb_options_,
                                   table_options_);
    return *db_;
  }

  // 흔한 2-속성 스키마 기본값(연속형 + 범주형).
  BitLSMOptions DefaultOptions() {
    BitLSMOptions options;
    options.attr_num = 2;
    options.attr_specs = {AttrType::ORDERED, AttrType::UNORDERED};
    options.read_seqno = 0;
    options.rho = 0.5;
    return options;
  }

  std::string db_path_;
  std::unique_ptr<BitLSM> db_;
  rocksdb::Options rocksdb_options_;
  rocksdb::BlockBasedTableOptions table_options_;
  rocksdb::Env* mem_env_ = nullptr;
};

}  // namespace bit_lsm
