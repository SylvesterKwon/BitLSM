#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"
#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <set>
#include <string>
#include <vector>

using namespace bit_lsm;

namespace {
std::set<std::string> ScanKeys(BitLSM& db, BitLSMQuery& query) {
  auto it = db.NewIterator(query);
  std::set<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    keys.insert(it->key().ToString());
  return keys;
}
}  // namespace

// memtable 안에서 연속형 범위 쿼리(a0 >= 10) → pk1, pk3.
TEST_F(BitLSMTestBase, FilteredQueryInMemtable) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  BitLSMQuery query(std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk1", "pk3"}));
}

// flush 로 SST 를 만든 뒤 같은 쿼리 → SABI 비트맵 인덱스 경로에서도 동일 결과.
TEST_F(BitLSMTestBase, FilteredQueryAfterFlush) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  // memtable → SST. FlushOptions 기본값은 완료까지 대기.
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));

  BitLSMQuery query(std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk1", "pk3"}));
}
