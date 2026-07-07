#include <gtest/gtest.h>
#include <rocksdb/db.h>

#include <set>
#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;

namespace {
std::set<std::string> ScanKeys(BitLSM& db, BitLSMQuery& query) {
  auto it = db.NewIterator(query);
  std::set<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    keys.insert(it->key().ToString());
  return keys;
}

// 2-attr schema: signed int32 ORDERED + UNORDERED string.
BitLSMOptions IntSchema() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 4, /*is_signed=*/true,
                           /*is_float=*/false),
                  AttrSpec{AttrRole::UNORDERED}};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}
}  // namespace

// memtable 안에서 연속형 범위 쿼리(a0 >= 10) → pk1, pk3.
TEST_F(BitLSMTestBase, FilteredQueryInMemtable) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk1", "pk3"}));
}

// Workload: a signed native-int (int32) ORDERED attr, range query crossing
// zero, evaluated in the memtable.
// Threat: the int comparand routed through the double path, or a re-check
// comparing the wrong native type, would drop negatives or misorder near zero.
TEST_F(BitLSMTestBase, NativeIntRangeInMemtable) {
  auto& db = OpenDB(IntSchema());
  BITLSM_ASSERT_OK(db.Put("pk1", {int64_t{-100}, std::string("a")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {int64_t{-5}, std::string("b")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {int64_t{0}, std::string("a")}, "p3"));
  BITLSM_ASSERT_OK(db.Put("pk4", {int64_t{42}, std::string("b")}, "p4"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, int64_t{-5}}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk2", "pk3", "pk4"}));
}

// Workload: same signed-int range plus a LESS query after a flush, so the
// answer flows through the SABI bitmap path: int values are projected to the
// double binning domain, and candidate rows are re-checked in the native
// int64 domain.
// Threat: sign lost in binning projection, or the bin-boundary math / native
// re-check disagreeing around zero.
TEST_F(BitLSMTestBase, NativeIntRangeAfterFlush) {
  auto& db = OpenDB(IntSchema());
  BITLSM_ASSERT_OK(db.Put("pk1", {int64_t{-100}, std::string("a")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {int64_t{-5}, std::string("b")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {int64_t{0}, std::string("a")}, "p3"));
  BITLSM_ASSERT_OK(db.Put("pk4", {int64_t{42}, std::string("b")}, "p4"));
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));

  BitLSMQuery q1(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, int64_t{-5}}});
  EXPECT_EQ(ScanKeys(db, q1), (std::set<std::string>{"pk2", "pk3", "pk4"}));

  BitLSMQuery q2(std::vector<QueryCondition>{{0, CompareOp::LESS, int64_t{0}}});
  EXPECT_EQ(ScanKeys(db, q2), (std::set<std::string>{"pk1", "pk2"}));
}

// Workload: an unsigned native-int (uint32) ORDERED attr with a value beyond
// INT32_MAX, EQUAL-matched after a flush.
// Threat: an unsigned comparand misread as signed, or the uint64 re-check
// path never being exercised.
TEST_F(BitLSMTestBase, NativeUintEqualityAfterFlush) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 4, /*is_signed=*/false,
                           /*is_float=*/false)};
  o.read_seqno = 0;
  o.rho = 0.5;
  auto& db = OpenDB(o);
  BITLSM_ASSERT_OK(db.Put("pk1", {uint64_t{10}}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {uint64_t{4000000000ULL}}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {uint64_t{10}}, "p3"));
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));

  BitLSMQuery q(std::vector<QueryCondition>{
      {0, CompareOp::EQUAL, uint64_t{4000000000ULL}}});
  EXPECT_EQ(ScanKeys(db, q), (std::set<std::string>{"pk2"}));
}

// Workload: a nullable ORDERED attr with NULL rows; range/equality queries
// must auto-exclude NULLs (SQL 3VL) in the memtable and after a flush (NULL
// rows are in no value bin, and the per-row re-check consults the presence
// bit). A conjunction whose other clause a NULL row satisfies still excludes
// it, because the NULL clause is UNKNOWN.
// Threat: a NULL row matching a range/equality it has no value for.
TEST_F(BitLSMTestBase, NullRowsExcludedFromQueries) {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, true, /*nullable=*/true),
                  AttrSpec{AttrRole::UNORDERED}};
  o.read_seqno = 0;
  o.rho = 0.5;
  auto& db = OpenDB(o);
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("a")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {std::monostate{}, std::string("b")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("a")}, "p3"));
  BITLSM_ASSERT_OK(db.Put("pk4", {std::monostate{}, std::string("a")}, "p4"));

  BitLSMQuery ge(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  // a1='a' AND a0>=10 : pk4 has category 'a' but a NULL a0, so it is excluded.
  BitLSMQuery conj(
      std::vector<OrClause>{{{1, CompareOp::EQUAL, std::string("a")}},
                            {{0, CompareOp::GREATER_EQUAL, 10.0}}});

  EXPECT_EQ(ScanKeys(db, ge), (std::set<std::string>{"pk1", "pk3"}));
  EXPECT_EQ(ScanKeys(db, conj), (std::set<std::string>{"pk1", "pk3"}));

  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));
  EXPECT_EQ(ScanKeys(db, ge), (std::set<std::string>{"pk1", "pk3"}));
  EXPECT_EQ(ScanKeys(db, conj), (std::set<std::string>{"pk1", "pk3"}));
}

// flush 로 SST 를 만든 뒤 같은 쿼리 → SABI 비트맵 인덱스 경로에서도 동일 결과.
TEST_F(BitLSMTestBase, FilteredQueryAfterFlush) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  // memtable → SST. FlushOptions 기본값은 완료까지 대기.
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk1", "pk3"}));
}
