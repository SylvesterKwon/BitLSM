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

// 2-attr schema: signed int32 kRange + kEquality string.
BitLSMOptions IntSchema() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kInt, 4),
                  AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}
}  // namespace

// Range query (a0 >= 10) against the memtable -> pk1, pk3.
TEST_F(BitLSMTestBase, FilteredQueryInMemtable) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk1", "pk3"}));
}

// A signed int32 range crossing zero, evaluated in the memtable.
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

// The same range after a flush: int values go through the bitmap path and
// candidates are re-checked in the native int64 domain.
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

// A uint32 value beyond INT32_MAX must not be misread as signed.
TEST_F(BitLSMTestBase, NativeUintEqualityAfterFlush) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(IndexType::kRange, PhysicalType::kUint, 4)};
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

// NULL rows are in no value bin and the per-row check consults the
// presence bit, so range and equality queries exclude them (3VL). A
// conjunction whose other clause a NULL row satisfies still excludes it.
TEST_F(BitLSMTestBase, NullRowsExcludedFromQueries) {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8, /*nullable=*/true),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
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

// Same query after a flush: the SABI bitmap path gives the same result.
TEST_F(BitLSMTestBase, FilteredQueryAfterFlush) {
  auto& db = OpenDB(DefaultOptions());
  BITLSM_ASSERT_OK(db.Put("pk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("pk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(db.Put("pk3", {25.0, std::string("apple")}, "p3"));

  // memtable -> SST; default FlushOptions wait for completion.
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));

  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(ScanKeys(db, query), (std::set<std::string>{"pk1", "pk3"}));
}
