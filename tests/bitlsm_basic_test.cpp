#include <gtest/gtest.h>
#include <rocksdb/db.h>

#include <set>
#include <string>
#include <string_view>

#include "bit_lsm_utils.h"
#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;

// Put 한 값이 실제 RocksDB 에 들어가고, raw Get 으로 꺼낸 인코딩 값이
// 우리가 넣은 속성으로 정확히 디코딩되는지. (fixture 의 디스크 격리도 검증)
TEST_F(BitLSMTestBase, PutThenRawGetRoundTrip) {
  BitLSMOptions opt = DefaultOptions();
  auto& db = OpenDB(opt);
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "payload1"));

  std::string raw;
  BITLSM_ASSERT_OK(
      db.GetInternalDB()->Get(rocksdb::ReadOptions(), "pk1", &raw));

  std::string_view buf(raw);
  EXPECT_DOUBLE_EQ(std::get<double>(DecodeAttr(opt, buf, 0)), 15.0);
  EXPECT_EQ(std::get<std::string_view>(DecodeAttr(opt, buf, 1)), "apple");
}

// 빈 쿼리로 스캔하면 넣은 모든 행이 나오는지. (iterator 배관 검증)
TEST_F(BitLSMTestBase, EmptyQueryScanReturnsAllRows) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));

  BitLSMQuery query;  // 빈 쿼리 → 전체 매칭
  auto it = db.NewIterator(query);
  std::set<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    keys.insert(it->key().ToString());

  EXPECT_EQ(keys, (std::set<std::string>{"pk1", "pk2"}));
}
