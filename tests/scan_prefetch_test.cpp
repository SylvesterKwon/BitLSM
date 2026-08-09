#include <fcntl.h>
#include <gtest/gtest.h>
#include <rocksdb/file_system.h>
#include <rocksdb/statistics.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bit_lsm_iterator.h"
#include "test_util/bitlsm_test_base.h"
#ifndef NDEBUG
#include "test_util/sync_point.h"
#endif

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

// Write rows [start, start+n) with sequential ordered-attr values and a
// padded payload so one Flush yields a single SST spanning many data blocks.
void FillRows(BitLSM& db, int n, int start = 0) {
  const std::string payload(32, 'p');
  for (int i = start; i < start + n; ++i) {
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

// Write rows [0, n) whose attribute alternates between 0 and 1 every
// `group` rows, so a query for the 0 group leaves periodic runs of
// non-matching data blocks between the target blocks.
void FillAlternatingRows(BitLSM& db, int n, int group) {
  const std::string payload(32, 'p');
  for (int i = 0; i < n; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "k%05d", i);
    const double attr = (i / group) % 2;
    ASSERT_TRUE(db.Put(key, {attr}, payload).ok());
  }
  ASSERT_TRUE(db.Flush().ok());
}

// Drain a scan of the 0 group written by FillAlternatingRows.
std::vector<std::pair<std::string, std::string>> ScanZeroGroup(BitLSM& db) {
  BitLSMQuery query(
      std::vector<QueryCondition>{{0, CompareOp::LESS_EQUAL, 0.0}});
  auto it = db.NewIterator(query);
  std::vector<std::pair<std::string, std::string>> rows;
  for (it->SeekToFirst(); it->Valid(); it->Next())
    rows.emplace_back(it->key().ToString(), it->value().ToString());
  EXPECT_TRUE(it->status().ok());
  return rows;
}

// Read requests and bytes a scan issued against SST files, so a test can
// assert on the shape of the I/O instead of on wall time.
struct ReadCounters {
  std::atomic<uint64_t> reads{0};
  std::atomic<uint64_t> bytes{0};
  void Reset() {
    reads.store(0);
    bytes.store(0);
  }
};

class CountingRandomAccessFile
    : public rocksdb::FSRandomAccessFileOwnerWrapper {
 public:
  CountingRandomAccessFile(
      std::unique_ptr<rocksdb::FSRandomAccessFile>&& target,
      ReadCounters* counters)
      : rocksdb::FSRandomAccessFileOwnerWrapper(std::move(target)),
        counters_(counters) {}

  rocksdb::IOStatus Read(uint64_t offset, size_t n,
                         const rocksdb::IOOptions& options,
                         rocksdb::Slice* result, char* scratch,
                         rocksdb::IODebugContext* dbg) const override {
    counters_->reads.fetch_add(1);
    counters_->bytes.fetch_add(n);
    return rocksdb::FSRandomAccessFileOwnerWrapper::Read(offset, n, options,
                                                         result, scratch, dbg);
  }

  rocksdb::IOStatus MultiRead(rocksdb::FSReadRequest* reqs, size_t num_reqs,
                              const rocksdb::IOOptions& options,
                              rocksdb::IODebugContext* dbg) override {
    counters_->reads.fetch_add(num_reqs);
    for (size_t i = 0; i < num_reqs; ++i)
      counters_->bytes.fetch_add(reqs[i].len);
    return rocksdb::FSRandomAccessFileOwnerWrapper::MultiRead(reqs, num_reqs,
                                                              options, dbg);
  }

 private:
  ReadCounters* counters_;
};

// Counts reads of SST files only; WAL, MANIFEST and CURRENT traffic is
// unrelated to the scan's block I/O and would only add noise.
class CountingFileSystem : public rocksdb::FileSystemWrapper {
 public:
  CountingFileSystem(std::shared_ptr<rocksdb::FileSystem> base,
                     ReadCounters* counters)
      : rocksdb::FileSystemWrapper(std::move(base)), counters_(counters) {}

  const char* Name() const override { return "CountingFileSystem"; }

  rocksdb::IOStatus NewRandomAccessFile(
      const std::string& fname, const rocksdb::FileOptions& opts,
      std::unique_ptr<rocksdb::FSRandomAccessFile>* result,
      rocksdb::IODebugContext* dbg) override {
    std::unique_ptr<rocksdb::FSRandomAccessFile> target;
    rocksdb::IOStatus s = rocksdb::FileSystemWrapper::NewRandomAccessFile(
        fname, opts, &target, dbg);
    if (!s.ok()) return s;
    const std::string_view name(fname);
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".sst")
      *result = std::make_unique<CountingRandomAccessFile>(std::move(target),
                                                           counters_);
    else
      *result = std::move(target);
    return s;
  }

 private:
  ReadCounters* counters_;
};

// A target-block list of `count` blocks of `block_size` bytes each, laid out
// so that consecutive targets sit `stride` block slots apart: stride 1 is a
// fully contiguous run, stride 2 leaves one non-target block between targets.
std::vector<std::pair<uint32_t, rocksdb::BlockHandle>> TargetBlocks(
    uint32_t count, uint64_t block_size, uint32_t stride) {
  std::vector<std::pair<uint32_t, rocksdb::BlockHandle>> targets;
  targets.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t offset = static_cast<uint64_t>(i) * stride * block_size;
    targets.emplace_back(i * stride, rocksdb::BlockHandle(offset, block_size));
  }
  return targets;
}

constexpr size_t k256K = 256 * 1024;

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

// Workload: rows compacted into several small L1+ SST files (tiny
//           target_file_size_base); a full-range scan walks them through
//           BitLSMLevelIterator in file order.
// Threat: the adaptive-readahead ramp restarts from scratch on every file —
//         readahead state is never carried across SSTs, so a level scan pays
//         the single-block warmup and the 8K-up ramp once per file instead of
//         once per level.
TEST_F(BitLSMTestBase, LevelScanCarriesReadaheadAcrossFiles) {
#ifdef NDEBUG
  GTEST_SKIP() << "sync points require a debug build";
#else
  // Direct I/O so the readahead goes through FilePrefetchBuffer: on buffered
  // POSIX the ramp is served by FS prefetch (fadvise) without a buffer, and
  // there is no state object to carry across files (same as vanilla).
  const char* mem = std::getenv("MEM_ENV");
  if (mem && std::string_view(mem) == "1")
    GTEST_SKIP() << "direct I/O is not supported on MemEnv";
  if (!DirectIOSupported(db_path_))
    GTEST_SKIP() << "filesystem rejects O_DIRECT";
  rocksdb_options_.use_direct_reads = true;

  table_options_.block_size = 512;
  // Pin the readahead ceiling below the smallest window the scan planner will
  // open, so this scan runs on the implicit adaptive ramp — the only path
  // whose state is worth carrying between files. Anything larger and the
  // planner picks an explicit window sized from each file's own target list,
  // which needs no warmup to carry forward.
  table_options_.max_auto_readahead_size = 4096;

  BitLSMOptions opt = ContOpt();
  BitLSM& db = OpenDB(opt);
  // Build 4 disjoint-range files on the bottom level: each chunk is flushed
  // and compacted alone, and since it overlaps nothing already there, it
  // lands as its own SST instead of merging.
  rocksdb::CompactRangeOptions cro;
  for (int chunk = 0; chunk < 4; ++chunk) {
    FillRows(db, 2500, chunk * 2500);
    ASSERT_TRUE(db.GetInternalDB()->CompactRange(cro, nullptr, nullptr).ok());
  }

  std::atomic<int> transfers{0};
  rocksdb::SyncPoint::GetInstance()->SetCallBack(
      "BlockPrefetcher::SetReadaheadState",
      [&transfers](void*) { transfers.fetch_add(1); });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();
  auto rows = ScanAll(db);
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  rocksdb::SyncPoint::GetInstance()->ClearAllCallBacks();

  ASSERT_EQ(rows.size(), 10000u);
  EXPECT_GT(transfers.load(), 0)
      << "readahead state never carried across files";
#endif
}

// Workload: a target list of 500 back-to-back 4K blocks, i.e. the scan
//           already knows it will read one contiguous 2MB run.
// Threat: the window chooser leaves readahead off for a run it can plainly
//         see is contiguous, so the scan pays one read request per block.
TEST(ScanReadaheadWindow, ContiguousTargetsPickTheLargestWindow) {
  EXPECT_EQ(
      ChooseScanReadaheadSize(TargetBlocks(500, 4096, /*stride=*/1), k256K),
      k256K);
}

// Workload: 500 4K target blocks with one non-target block between each pair,
//           the density at which RocksDB's implicit adaptive readahead gives
//           up because its sequential check demands exact adjacency.
// Threat: the chooser inherits the same all-or-nothing adjacency rule and
//         disables readahead over a gap that costs far less to read than the
//         request it saves.
TEST(ScanReadaheadWindow, TargetsSeparatedByOneBlockStillMerge) {
  EXPECT_EQ(
      ChooseScanReadaheadSize(TargetBlocks(500, 4096, /*stride=*/2), k256K),
      k256K);
}

// Workload: 500 4K target blocks 128 slots apart, so the 512KB hole between
//           consecutive targets costs more to read than the read request
//           skipping it would have cost.
// Threat: the chooser merges on gap-tolerance alone and reads an order of
//         magnitude more bytes than the request it saved is worth.
TEST(ScanReadaheadWindow, WidelySpacedTargetsLeaveReadaheadOff) {
  EXPECT_EQ(
      ChooseScanReadaheadSize(TargetBlocks(500, 4096, /*stride=*/128), k256K),
      0u);
}

// Workload: a contiguous target run under a table configured with a 64KB
//           max_auto_readahead_size.
// Threat: the explicit-readahead branch of PrefetchIfNeeded never consults
//         max_auto_readahead_size, so a window picked without clamping it
//         silently overrides the table's configured readahead ceiling.
TEST(ScanReadaheadWindow, WindowNeverExceedsTheConfiguredMaximum) {
  EXPECT_EQ(
      ChooseScanReadaheadSize(TargetBlocks(500, 4096, /*stride=*/1), 64 * 1024),
      64u * 1024u);
}

// Workload: a contiguous target run under max_auto_readahead_size = 0, the
//           setting that structurally disables readahead for a table.
// Threat: span readahead bypasses the kill switch, leaving no configuration
//         that turns scan readahead off — which also silently invalidates the
//         prefetch-on/off result-identity test.
TEST(ScanReadaheadWindow, ZeroMaximumDisablesReadahead) {
  EXPECT_EQ(ChooseScanReadaheadSize(TargetBlocks(500, 4096, /*stride=*/1), 0),
            0u);
}

// Workload: a contiguous target run under an absurd max_auto_readahead_size,
//           the largest value the option can hold.
// Threat: the candidate window doubles past the top of size_t, wraps to 0 and
//         never exceeds the ceiling again, hanging the scan's constructor.
TEST(ScanReadaheadWindow, AbsurdMaximumTerminatesWithASaneWindow) {
  const size_t window =
      ChooseScanReadaheadSize(TargetBlocks(500, 4096, /*stride=*/1), SIZE_MAX);
  EXPECT_GT(window, 0u);
  EXPECT_LE(window, 500u * 4096u) << "window exceeds the whole target span";
}

// Workload: a scan whose query bitmap resolved to a single target block.
// Threat: the chooser opens a readahead window with nothing left to merge
//         into it, turning a 4K read into a 260K one for no saved request.
TEST(ScanReadaheadWindow, SingleTargetLeavesReadaheadOff) {
  EXPECT_EQ(ChooseScanReadaheadSize(TargetBlocks(1, 4096, /*stride=*/1), k256K),
            0u);
}

// Workload: one Flushed SST of 8000 rows over 512B data blocks, scanned for
//           an attribute group that alternates every 64 rows, so target
//           blocks come in runs separated by runs of non-target blocks; the
//           same scan runs twice, once with readahead available and once with
//           max_auto_readahead_size = 0.
// Threat: gaps between target runs reset RocksDB's adjacency-based readahead
//         ramp, so the dense scan degenerates into one read request per
//         target block and reads at random-I/O rate instead of sequential
//         bandwidth.
TEST_F(BitLSMTestBase, GappyDenseScanCollapsesReadRequests) {
  ReadCounters counters;
  rocksdb::Env* base_env = mem_env_ ? mem_env_ : rocksdb::Env::Default();
  auto counting_fs = std::make_shared<CountingFileSystem>(
      base_env->GetFileSystem(), &counters);
  std::unique_ptr<rocksdb::Env> counting_env =
      rocksdb::NewCompositeEnv(counting_fs);
  rocksdb_options_.env = counting_env.get();
  table_options_.block_size = 512;

  BitLSMOptions opt = ContOpt();
  BitLSM& db_on = OpenDB(opt);
  FillAlternatingRows(db_on, 8000, /*group=*/64);
  counters.Reset();
  auto rows_on = ScanZeroGroup(db_on);
  const uint64_t reads_on = counters.reads.load();

  // Reopen the same DB with readahead structurally disabled.
  table_options_.max_auto_readahead_size = 0;
  BitLSM& db_off = OpenDB(opt);
  counters.Reset();
  auto rows_off = ScanZeroGroup(db_off);
  const uint64_t reads_off = counters.reads.load();

  db_.reset();  // before counting_env, which the DB borrows

  ASSERT_FALSE(rows_on.empty());
  ASSERT_EQ(rows_on.size(), rows_off.size());
  for (size_t i = 0; i < rows_on.size(); ++i) {
    ASSERT_EQ(rows_on[i].first, rows_off[i].first) << "key mismatch at " << i;
    ASSERT_EQ(rows_on[i].second, rows_off[i].second)
        << "value mismatch at " << i;
  }
  EXPECT_LT(reads_on * 5, reads_off)
      << "gappy dense scan issued " << reads_on << " read requests with "
      << "readahead against " << reads_off << " without";
}
