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

// SABI-domain conditions for the estimator API. Both schemas here leave
// AttrSpec at its f64 default, so ORDERED comparands encode through F64ToOkey.
SABICondition OrdF64(uint32_t attr, CompareOp op, double v) {
  return SABICondition{attr, op, OkeyInterval::FromOp(op, F64ToOkey(v)), ""};
}

SABICondition Uno(uint32_t attr, const std::string& v) {
  return SABICondition{attr, CompareOp::EQUAL, {}, v};
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

// Workload: a CF created at runtime, written, flushed, queried, then dropped.
// Threat: CreateColumnFamily not installing the CF's own SABI factory (flush
// would produce an SST the scan path can't prune/read), or Drop leaving a
// stale registry entry behind.
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

// Workload: the same primary key written to two CFs with different payloads
// -- the orders side through the cf-first PutBatch, alongside a second orders
// key -- then one key deleted in each CF through that CF's own handle.
// Threat: CF keyspaces bleeding into each other -- a shared key resolving to
// the other CF's row (or deleting in one CF erasing the other's row).
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

// Workload: DB written with two CFs, closed, reopened -- once with the full
// descriptor list, once with the default CF only.
// Threat: RocksDB's must-list-all-CFs rule getting swallowed (silent data
// loss on partial reopen), or per-CF rows/schemas not surviving reopen.
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

// Workload: two CFs, both with the estimator enabled; only "orders" gets
// rows and a flush, then both estimators are refreshed and asked.
// Threat: a single shared estimator (or one bound to the wrong cfd) would
// report orders' rows under the default CF, or leave orders on fallback.
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

// Workload: two CFs with the estimator enabled; rows written to "orders" and
// flushed, then the estimate is polled -- with no TEST_Refresh anywhere --
// until the flushed SST shows up in that CF's stats.
// Threat: StatsRefreshListener routes flush/compaction events by cf id, and
// every other estimator test signals the worker directly through
// TEST_Refresh. A dropped event, or one delivered to the wrong CF's
// estimator, would leave planning stats silently stale forever; only the
// auto-refresh path can observe it.
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
  // The refresh is asynchronous by design (bounded staleness), so poll rather
  // than read once; the bound only has to outlast a healthy listener hop.
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

// Workload: every cf-first entry point handed the nullptr that
// GetColumnFamily returns for a name the DB does not have.
// Threat: composing GetColumnFamily("typo") with an op must degrade to an
// error status, a null iterator or a fallback estimate -- never a null
// dereference inside the write/scan path.
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
