#pragma once

#include <gtest/gtest.h>
#include <rocksdb/status.h>

// rocksdb::Status assertions that print the Status message on failure.
// Prefixed to avoid clashing with ASSERT_OK/EXPECT_OK from RocksDB's
// test_util/testharness.h.
#define BITLSM_ASSERT_OK(s) ASSERT_TRUE((s).ok()) << (s).ToString()
#define BITLSM_EXPECT_OK(s) EXPECT_TRUE((s).ok()) << (s).ToString()
