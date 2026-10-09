#include <gtest/gtest.h>
#include <rocksdb/db.h>

#include <chrono>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "bit_lsm_encoding.h"
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

// 4-attr schema, different from DefaultOptions()'s 2-attr schema so a
// cross-CF decode mix-up shows.
BitLSMOptions FourAttrOptions() {
  BitLSMOptions options;
  options.attr_num = 4;
  options.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  options.read_seqno = 0;
  options.rho = 0.5;
  return options;
}

// SABI-domain conditions for the estimator API. Both schemas here leave
// AttrSpec at its f64 default, so kRange comparands encode through F64ToOkey.
SABICondition OrdF64(uint32_t attr, CompareOp op, double v) {
  return SABICondition{attr,
                       ByteInterval::FromOp(op, OkeyToBytes(F64ToOkey(v))), ""};
}

SABICondition Uno(uint32_t attr, const std::string& v) {
  return SABICondition{attr, {}, v};
}
}  // namespace

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

TEST_F(BitLSMTestBase, OpenWithoutDefaultDescriptorThrows) {
  EXPECT_THROW(OpenDB({{"orders", FourAttrOptions()}}), std::invalid_argument);
}

// RocksDB's Open does not reject duplicate names itself.
TEST_F(BitLSMTestBase, OpenWithDuplicateDescriptorThrows) {
  EXPECT_THROW(OpenDB({{rocksdb::kDefaultColumnFamilyName, DefaultOptions()},
                       {"orders", FourAttrOptions()},
                       {"orders", FourAttrOptions()}}),
               std::invalid_argument);
}

TEST_F(BitLSMTestBase, CreateAndDropColumnFamilyAtRuntime) {
  auto& db = OpenDB(DefaultOptions());

  ColumnFamilyHandle* aux = nullptr;
  BITLSM_ASSERT_OK(db.CreateColumnFamily("aux", FourAttrOptions(), &aux));
  ASSERT_NE(aux, nullptr);
  EXPECT_EQ(db.GetColumnFamily("aux"), aux);

  BITLSM_ASSERT_OK(
      db.Put(aux, "ak1", {15.0, 1.0, 2.0, std::string("x")}, "pa"));
  BITLSM_ASSERT_OK(db.Flush(aux));

  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  auto it = db.NewIterator(aux, q);
  ASSERT_NE(it, nullptr);
  EXPECT_EQ(CollectKeys(it.get()), (std::set<std::string>{"ak1"}));

  // Duplicate name and default-CF drop are refused.
  ColumnFamilyHandle* dup = nullptr;
  EXPECT_FALSE(db.CreateColumnFamily("aux", FourAttrOptions(), &dup).ok());
  EXPECT_FALSE(db.DropColumnFamily(db.DefaultColumnFamily()).ok());

  BITLSM_ASSERT_OK(db.DropColumnFamily(aux));
  EXPECT_EQ(db.GetColumnFamily("aux"), nullptr);
}

TEST_F(BitLSMTestBase, SameKeyIsIndependentAcrossColumnFamilies) {
  auto& db = OpenDB({{rocksdb::kDefaultColumnFamilyName, DefaultOptions()},
                     {"orders", FourAttrOptions()}});
  ColumnFamilyHandle* orders = db.GetColumnFamily("orders");

  BITLSM_ASSERT_OK(db.Put("shared", {15.0, std::string("apple")}, "pd"));
  std::vector<std::string> order_keys{"shared", "ok2"};
  std::vector<std::vector<Attr>> order_attrs{
      {15.0, 1.0, 2.0, std::string("x")}, {20.0, 3.0, 4.0, std::string("y")}};
  std::vector<std::string> order_payloads{"po", "po2"};
  BITLSM_ASSERT_OK(
      db.PutBatch(orders, order_keys, order_attrs, order_payloads));
  BITLSM_ASSERT_OK(db.Flush());
  BITLSM_ASSERT_OK(db.Flush(orders));

  BITLSM_ASSERT_OK(db.Delete("shared"));
  BITLSM_ASSERT_OK(db.Delete(orders, "ok2"));

  BitLSMQuery q_orders(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  auto oit = db.NewIterator(orders, q_orders);
  ASSERT_NE(oit, nullptr);
  // "shared" survives the default-CF delete of the same key; "ok2" matches the
  // predicate on its own merits but not its own CF's delete.
  EXPECT_EQ(CollectKeys(oit.get()), (std::set<std::string>{"shared"}));

  BitLSMQuery q_default(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  auto dit = db.NewIterator(q_default);
  ASSERT_NE(dit, nullptr);
  EXPECT_EQ(CollectKeys(dit.get()), (std::set<std::string>{}));
}

TEST_F(BitLSMTestBase, ReopenRequiresFullDescriptorList) {
  {
    auto& db = OpenDB({{rocksdb::kDefaultColumnFamilyName, DefaultOptions()},
                       {"orders", FourAttrOptions()}});
    ColumnFamilyHandle* orders = db.GetColumnFamily("orders");
    BITLSM_ASSERT_OK(
        db.Put(orders, "ok1", {15.0, 1.0, 2.0, std::string("x")}, "po1"));
    BITLSM_ASSERT_OK(db.Flush(orders));
    db_.reset();  // close before reading the manifest / reopening
  }

  std::vector<std::string> names;
  BITLSM_ASSERT_OK(
      BitLSM::ListColumnFamilies(rocksdb_options_, db_path_, &names));
  EXPECT_EQ(
      (std::set<std::string>(names.begin(), names.end())),
      (std::set<std::string>{rocksdb::kDefaultColumnFamilyName, "orders"}));

  EXPECT_THROW(OpenDB(DefaultOptions()), std::runtime_error);

  auto& db = OpenDB({{rocksdb::kDefaultColumnFamilyName, DefaultOptions()},
                     {"orders", FourAttrOptions()}});
  ColumnFamilyHandle* orders = db.GetColumnFamily("orders");
  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  auto it = db.NewIterator(orders, q);
  ASSERT_NE(it, nullptr);
  EXPECT_EQ(CollectKeys(it.get()), (std::set<std::string>{"ok1"}));
}

// Only "orders" gets rows; the default CF's estimator must stay on
// fallback.
TEST_F(BitLSMTestBase, EstimatorBindsPerColumnFamily) {
  BitLSMOptions default_opts = DefaultOptions();
  default_opts.enable_estimator = true;
  default_opts.estimator_min_rebuild_interval_ms = 0;  // deterministic refresh
  BitLSMOptions orders_opts = FourAttrOptions();
  orders_opts.enable_estimator = true;
  orders_opts.estimator_min_rebuild_interval_ms = 0;

  auto& db = OpenDB({{rocksdb::kDefaultColumnFamilyName, default_opts},
                     {"orders", orders_opts}});
  ColumnFamilyHandle* orders = db.GetColumnFamily("orders");
  ASSERT_NE(orders->Estimator(), nullptr);
  ASSERT_NE(db.DefaultColumnFamily()->Estimator(), nullptr);

  for (int i = 0; i < 100; ++i)
    BITLSM_ASSERT_OK(db.Put(orders, "ok" + std::to_string(i),
                            {double(i), 1.0, 2.0, std::string("x")}, "p"));
  BITLSM_ASSERT_OK(db.Flush(orders));
  orders->Estimator()->TEST_Refresh();
  db.DefaultColumnFamily()->Estimator()->TEST_Refresh();

  // attr 0 >= 0.0 over the 4-attr schema: matches all 100 orders rows.
  SABIQuery q_orders;
  q_orders.clause_groups = {{OrdF64(0, CompareOp::GREATER_EQUAL, 0.0)}};
  EstimateResult r = db.EstimateSelectivity(orders, q_orders);
  EXPECT_EQ(r.physical_rows, 100u);
  EXPECT_TRUE(r.fallback_attrs.empty());
  EXPECT_NEAR(r.selectivity * static_cast<double>(r.physical_rows), 100.0,
              1e-6);

  // Same DB, default (2-attr) schema: that CF has no live SST, so both
  // queried attrs fall back and orders' rows are nowhere in sight.
  SABIQuery q_default;
  q_default.clause_groups = {{OrdF64(0, CompareOp::GREATER_EQUAL, 0.0)},
                             {Uno(1, "apple")}};
  EstimateResult d = db.EstimateSelectivity(q_default);
  EXPECT_EQ(d.physical_rows, 0u);
  EXPECT_DOUBLE_EQ(d.selectivity, 1.0);
  EXPECT_EQ(d.fallback_attrs, (std::vector<uint32_t>{0, 1}));
}

// Polls without TEST_Refresh, so a flush event routed to the wrong CF
// would show up as stale stats.
TEST_F(BitLSMTestBase, StatsListenerRoutesFlushToOwningColumnFamily) {
  BitLSMOptions default_opts = DefaultOptions();
  default_opts.enable_estimator = true;
  default_opts.estimator_min_rebuild_interval_ms = 0;
  BitLSMOptions orders_opts = FourAttrOptions();
  orders_opts.enable_estimator = true;
  orders_opts.estimator_min_rebuild_interval_ms = 0;

  auto& db = OpenDB({{rocksdb::kDefaultColumnFamilyName, default_opts},
                     {"orders", orders_opts}});
  ColumnFamilyHandle* orders = db.GetColumnFamily("orders");
  ASSERT_NE(orders, nullptr);

  for (int i = 0; i < 100; ++i)
    BITLSM_ASSERT_OK(db.Put(orders, "ok" + std::to_string(i),
                            {double(i), 1.0, 2.0, std::string("x")}, "p"));
  BITLSM_ASSERT_OK(db.Flush(orders));

  SABIQuery q;
  q.clause_groups = {{OrdF64(0, CompareOp::GREATER_EQUAL, 0.0)}};
  // The refresh is asynchronous, so poll rather than read once.
  EstimateResult r;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    r = db.EstimateSelectivity(orders, q);
    if (r.physical_rows == 100u) break;
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(r.physical_rows, 100u)
      << "orders' estimator never saw its own flush";
  EXPECT_TRUE(r.fallback_attrs.empty());
}

// Every cf-first entry point gets the nullptr GetColumnFamily returns for
// an unknown name.
TEST_F(BitLSMTestBase, NullColumnFamilyHandleIsRejected) {
  auto& db = OpenDB(DefaultOptions());
  ColumnFamilyHandle* missing = db.GetColumnFamily("typo");
  ASSERT_EQ(missing, nullptr);

  EXPECT_TRUE(
      db.Put(missing, "k", {1.0, std::string("a")}, "p").IsInvalidArgument());
  std::vector<std::string> keys{"k"};
  std::vector<std::vector<Attr>> attrs{{1.0, std::string("a")}};
  std::vector<std::string> payloads{"p"};
  EXPECT_TRUE(db.PutBatch(missing, keys, attrs, payloads).IsInvalidArgument());
  EXPECT_TRUE(db.Delete(missing, "k").IsInvalidArgument());
  EXPECT_TRUE(db.Flush(missing).IsInvalidArgument());

  BitLSMQuery q(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 10.0}});
  EXPECT_EQ(db.NewIterator(missing, q), nullptr);

  SABIQuery sq;
  sq.clause_groups = {{OrdF64(0, CompareOp::GREATER_EQUAL, 0.0)}};
  EstimateResult r = db.EstimateSelectivity(missing, sq);
  EXPECT_EQ(r.physical_rows, 0u);
  EXPECT_EQ(r.fallback_attrs, (std::vector<uint32_t>{0}));
}
