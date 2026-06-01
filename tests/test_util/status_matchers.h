#pragma once

#include <gtest/gtest.h>
#include <rocksdb/status.h>

// rocksdb::Status 를 검사하는 매처. 실패 시 Status 메시지를 출력한다.
// 이름을 BITLSM_ 접두사로 둬서 RocksDB 의 test_util/testharness.h 가 정의하는
// ASSERT_OK/EXPECT_OK 와의 충돌을 원천 차단한다.
#define BITLSM_ASSERT_OK(s) ASSERT_TRUE((s).ok()) << (s).ToString()
#define BITLSM_EXPECT_OK(s) EXPECT_TRUE((s).ok()) << (s).ToString()
