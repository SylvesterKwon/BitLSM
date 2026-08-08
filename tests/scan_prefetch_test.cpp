#include <fcntl.h>
#include <gtest/gtest.h>
#include <rocksdb/statistics.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_util/bitlsm_test_base.h"

using namespace bit_lsm;

namespace {

BitLSMOptions ContOpt() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec{AttrRole::ORDERED}};
  o.read_seqno = 0;
  o.rho = 0.5;
  return o;
}

// True when the filesystem holding `dir` accepts O_DIRECT opens (tmpfs does
// not), so tests that require use_direct_reads can skip instead of failing.
bool DirectIOSupported(const std::string& dir) {
  std::string probe = dir + "_odirect_probe";
  int wfd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd < 0) return false;
  ::close(wfd);
  int rfd = ::open(probe.c_str(), O_RDONLY | O_DIRECT);
  bool ok = rfd >= 0;
  if (rfd >= 0) ::close(rfd);
  ::remove(probe.c_str());
  return ok;
}

// Write `n` rows with sequential ordered-attr values and a padded payload so
// one Flush yields a single SST spanning many data blocks.
void FillRows(BitLSM& db, int n) {
  const std::string payload(32, 'p');
  for (int i = 0; i < n; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "k%05d", i);
    ASSERT_TRUE(db.Put(key, {static_cast<double>(i)}, payload).ok());
  }
  ASSERT_TRUE(db.Flush().ok());
}

// Drain a full-range scan (attr >= 0 matches every row) and return all rows.
std::vector<std::pair<std::string, std::string>> ScanAll(BitLSM& db) {
  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 0.0}});
  auto it = db.NewIterator(query);
  std::vector<std::pair<std::string, std::string>> rows;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    rows.emplace_back(it->key().ToString(), it->value().ToString());
  EXPECT_TRUE(it->status().ok());
  return rows;
}

}  // namespace

// Workload: one Flushed SST with 2000 rows across many 512B data blocks
//           (direct I/O, statistics on); a full-range query makes every block
//           a target, i.e. the coarse-rho dense-scan pattern.
// Threat: the SABI scan path reads target blocks as synchronous single-block
//         I/O with no readahead — RocksDB's FilePrefetchBuffer never engages,
//         so PREFETCH_BYTES stays 0 and a dense scan pays a random-read
//         penalty per block instead of sequential bandwidth.
TEST_F(BitLSMTestBase, DenseScanEngagesPrefetch) {
  const char* mem = std::getenv("MEM_ENV");
  if (mem && std::string_view(mem) == "1")
    GTEST_SKIP() << "direct I/O is not supported on MemEnv";
  if (!DirectIOSupported(db_path_))
    GTEST_SKIP() << "filesystem rejects O_DIRECT";

  rocksdb_options_.statistics = rocksdb::CreateDBStatistics();
  rocksdb_options_.use_direct_reads = true;
  table_options_.block_size = 512;

  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  FillRows(db, 2000);

  auto rows = ScanAll(db);
  ASSERT_EQ(rows.size(), 2000u);
  EXPECT_GT(
      rocksdb_options_.statistics->getTickerCount(rocksdb::PREFETCH_BYTES), 0u)
      << "dense SABI scan performed no readahead";
}

// Workload: one Flushed SST with 2000 rows across many 512B data blocks; the
//           same full-range scan run twice — once with auto-readahead enabled
//           (default) and once structurally disabled via
//           max_auto_readahead_size = 0.
// Threat: wiring the prefetch buffer into the scan path alters what the scan
//         returns — keys or values diverge byte-wise between the prefetch and
//         no-prefetch configurations.
TEST_F(BitLSMTestBase, PrefetchOnOffResultsIdentical) {
  table_options_.block_size = 512;
  BitLSMOptions opt = ContOpt();

  BitLSM& db_on = OpenDB(opt);
  FillRows(db_on, 2000);
  auto rows_on = ScanAll(db_on);
  ASSERT_EQ(rows_on.size(), 2000u);

  // Reopen the same DB with implicit auto-readahead disabled.
  table_options_.max_auto_readahead_size = 0;
  BitLSM& db_off = OpenDB(opt);
  auto rows_off = ScanAll(db_off);

  ASSERT_EQ(rows_on.size(), rows_off.size());
  for (size_t i = 0; i < rows_on.size(); ++i) {
    ASSERT_EQ(rows_on[i].first, rows_off[i].first) << "key mismatch at " << i;
    ASSERT_EQ(rows_on[i].second, rows_off[i].second)
        << "value mismatch at " << i;
  }
}
