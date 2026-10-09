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
#include <vector>

#include "bit_lsm.h"
#include "bit_lsm_option.h"

namespace bit_lsm {

// Base fixture for e2e tests: a fresh temp DB directory per test, removed
// on teardown. MEM_ENV=1 runs on NewMemEnv instead of disk.
class BitLSMTestBase : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name =
        std::string(info->test_suite_name()) + "_" + info->name();
    for (char& c : name)
      if (c == '/') c = '_';  // parameterized test names contain '/'

    const char* tmp_env = std::getenv("TEST_TMPDIR");
    std::string tmproot = tmp_env ? tmp_env : ::testing::TempDir();
    if (!tmproot.empty() && tmproot.back() != '/') tmproot.push_back('/');
    db_path_ = tmproot + "bitlsm_" + name + "_" + std::to_string(getpid());

    const char* mem = std::getenv("MEM_ENV");
    if (mem && std::string_view(mem) == "1") {
      mem_env_ = rocksdb::NewMemEnv(rocksdb::Env::Default());
      rocksdb_options_.env = mem_env_;
    }

    // Remove leftovers of a crashed run.
    rocksdb::DestroyDB(db_path_, rocksdb::Options());
    rocksdb_options_.create_if_missing = true;
    // The directory starts empty, so CFs a test names must be created at
    // open (no effect on single-CF opens).
    rocksdb_options_.create_missing_column_families = true;
  }

  void TearDown() override {
    db_.reset();  // the BitLSM destructor closes the DB
    const char* keep = std::getenv("KEEP_DB");
    if (keep && std::string_view(keep) == "1") {
      std::cerr << "KEEP_DB: leaving DB at " << db_path_ << "\n";
    } else {
      rocksdb::Options opts;
      opts.env = mem_env_ ? mem_env_ : rocksdb::Env::Default();
      rocksdb::DestroyDB(db_path_, opts);
    }
    delete mem_env_;  // nullptr when unused
    mem_env_ = nullptr;
  }

  // Opens a BitLSM at db_path_, owned by the fixture. The reference is valid
  // until the next OpenDB() or TearDown. The previous DB is closed first so
  // the two never contend for the RocksDB LOCK file.
  BitLSM& OpenDB(const BitLSMOptions& bitlsm_options) {
    db_.reset();
    db_ = std::make_unique<BitLSM>(db_path_, bitlsm_options, rocksdb_options_,
                                   table_options_);
    return *db_;
  }

  // Multi-CF open at db_path_. Same ownership/lifetime rules as OpenDB().
  BitLSM& OpenDB(const std::vector<ColumnFamilyDescriptor>& descriptors) {
    db_.reset();
    db_ = std::make_unique<BitLSM>(db_path_, rocksdb_options_, table_options_,
                                   descriptors);
    return *db_;
  }

  // Same as OpenDB(), but installs custom BlockBasedTableOptions first and
  // leaves them installed for later OpenDB() calls in the same test.
  BitLSM& OpenDBWithTableOptions(
      const BitLSMOptions& bitlsm_options,
      const rocksdb::BlockBasedTableOptions& table_options) {
    table_options_ = table_options;
    return OpenDB(bitlsm_options);
  }

  // Common two-attr schema: one kRange, one kEquality.
  BitLSMOptions DefaultOptions() {
    BitLSMOptions options;
    options.attr_num = 2;
    options.attr_specs = {
        AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
        AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
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
