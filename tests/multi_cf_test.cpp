#include <gtest/gtest.h>
#include <rocksdb/db.h>

#include <set>
#include <string>
#include <vector>

#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;

namespace {
std::set<std::string> CollectKeys(BitLSMIterator* it) {
  std::set<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    keys.insert(it->key().ToString());
  return keys;
}

// 4-attr schema, deliberately shaped differently from DefaultOptions()'s
// 2-attr schema so cross-CF decode confusion cannot pass by accident.
BitLSMOptions FourAttrOptions() {
  BitLSMOptions options;
  options.attr_num = 4;
  options.attr_specs = {
      AttrSpec{AttrRole::ORDERED}, AttrSpec{AttrRole::ORDERED},
      AttrSpec{AttrRole::ORDERED}, AttrSpec{AttrRole::UNORDERED}};
  options.read_seqno = 0;
  options.rho = 0.5;
  return options;
}
}  // namespace

// Workload: one DB, two CFs with different schemas (2-attr default, 4-attr
// "orders"); rows written to each through its handle, flushed, then queried
// per CF on attr 0.
// Threat: ops falling through to the default CF (wrong handle plumbing), or a
// CF decoded with the other CF's schema — either surfaces the other CF's keys
// or none of its own.
TEST_F(BitLSMTestBase, TwoColumnFamiliesIsolateRowsAndSchemas) {
  auto& db = OpenDB({{rocksdb::kDefaultColumnFamilyName, DefaultOptions()},
                     {"orders", FourAttrOptions()}});
  ColumnFamilyHandle* orders = db.GetColumnFamily("orders");
  ASSERT_NE(orders, nullptr);
  ASSERT_NE(db.DefaultColumnFamily(), nullptr);
  EXPECT_EQ(db.GetColumnFamily("nope"), nullptr);

  BITLSM_ASSERT_OK(db.Put("dk1", {15.0, std::string("apple")}, "p1"));
  BITLSM_ASSERT_OK(db.Put("dk2", {5.0, std::string("banana")}, "p2"));
  BITLSM_ASSERT_OK(
      db.Put(orders, "ok1", {15.0, 1.0, 2.0, std::string("x")}, "po1"));
  BITLSM_ASSERT_OK(
      db.Put(orders, "ok2", {3.0, 4.0, 5.0, std::string("y")}, "po2"));
  BITLSM_ASSERT_OK(db.Flush());
  BITLSM_ASSERT_OK(db.Flush(orders));

  BitLSMQuery q_default(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  auto it = db.NewIterator(q_default);
  ASSERT_NE(it, nullptr);
  EXPECT_EQ(CollectKeys(it.get()), (std::set<std::string>{"dk1"}));

  BitLSMQuery q_orders(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  auto oit = db.NewIterator(orders, q_orders);
  ASSERT_NE(oit, nullptr);
  EXPECT_EQ(CollectKeys(oit.get()), (std::set<std::string>{"ok1"}));
}

// Workload: multi-CF open whose descriptor list omits the default CF.
// Threat: silently opening without a default CF would leave every CF-less
// convenience op with no target; this must fail loudly at open.
TEST_F(BitLSMTestBase, OpenWithoutDefaultDescriptorThrows) {
  EXPECT_THROW(OpenDB({{"orders", FourAttrOptions()}}), std::invalid_argument);
}

// Workload: multi-CF open whose descriptor list names the same CF twice.
// Threat: RocksDB's Open does not reject duplicate names -- it returns two
// handles to one CF; without validation the second registry insert silently
// destroys a live handle while the stats listener still references its
// estimator (use-after-free, unclean close).
TEST_F(BitLSMTestBase, OpenWithDuplicateDescriptorThrows) {
  EXPECT_THROW(OpenDB({{rocksdb::kDefaultColumnFamilyName, DefaultOptions()},
                       {"orders", FourAttrOptions()},
                       {"orders", FourAttrOptions()}}),
               std::invalid_argument);
}
