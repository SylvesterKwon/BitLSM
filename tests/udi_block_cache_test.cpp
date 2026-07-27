#include <gtest/gtest.h>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/table.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_query.h"
#include "sabi.h"
#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

// 10k rows: a0 = "c<i%10>" (unordered), a1 = i%1000 (ordered).
constexpr int kRows = 10000;
// a0 == "c3" AND a1 < 500  ->  i%10==3 && i%1000<500  ->  50 per 1000-block,
// 10 blocks -> 500 rows.
constexpr size_t kExpectedMatches = 500;

BitLSMOptions TwoAttrOptions() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec{AttrRole::UNORDERED}, AttrSpec{AttrRole::ORDERED}};
  o.read_seqno = 0;  // each iterator overwrites this from its snapshot
  o.rho = 0.1;       // only affects bin counts; any sane value works here
  return o;
}

BitLSMQuery ConjunctiveQuery() {
  return BitLSMQuery(std::vector<QueryCondition>{
      {0, CompareOp::EQUAL, std::string("c3")}, {1, CompareOp::LESS, 500.0}});
}

// Matches nothing: the iterator still has to pin the SABI entry to find that
// out, but never loads a data block -- so a pinned data block cannot mask the
// SABI pin in the cache accounting.
BitLSMQuery NoMatchQuery() {
  return BitLSMQuery(std::vector<QueryCondition>{
      {0, CompareOp::EQUAL, std::string("no-such-category")}});
}

// A SABI blob plus the reader parsed out of it. The builder owns the blob's
// memory and OnKeyAdded only borrows the encoded rows, so all three have to
// stay alive together for the reader to remain usable.
struct BuiltIndex {
  std::unique_ptr<SABIBuilder> builder;
  std::vector<std::string> encoded;
  std::unique_ptr<SABIReader> reader;
};

// Builds an index over `rows` rows whose unordered attr cycles through
// `distinct` long (non-SSO) values, then parses it back.
BuiltIndex BuildIndex(int rows, int distinct) {
  BitLSMOptions o = TwoAttrOptions();
  BuiltIndex bi;
  bi.builder = std::make_unique<SABIBuilder>(
      SABISchema::FromOptions(o), std::make_unique<ValueLayoutExtractor>(o));

  std::vector<std::string> keys;
  keys.reserve(rows);
  bi.encoded.reserve(rows);
  for (int i = 0; i < rows; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "k%08d", i);
    keys.emplace_back(key);
    std::string out;
    EncodeValue(o,
                {std::string("value-well-past-the-sso-budget-") +
                     std::to_string(i % distinct),
                 static_cast<double>(i)},
                "p", out);
    bi.encoded.push_back(std::move(out));
    bi.builder->OnKeyAdded(rocksdb::Slice(keys[i]), UDIB::ValueType::kValue,
                           rocksdb::Slice(bi.encoded[i]));
  }

  std::string scratch;
  UDIB::BlockHandle block_handle{/*offset=*/100, /*size=*/4096};
  bi.builder->AddIndexEntry(rocksdb::Slice(keys.back()),
                            /*first_key_in_next_block=*/nullptr, block_handle,
                            &scratch);
  rocksdb::Slice blob;
  EXPECT_TRUE(bi.builder->Finish(&blob).ok());
  bi.reader = std::make_unique<SABIReader>(blob);
  return bi;
}

}  // namespace

// A fixture whose block cache is created (and observable) per test, so
// GetUsage() reflects exactly this DB's blocks.
class UDIBlockCacheTest : public BitLSMTestBase {
 protected:
  void SetUp() override {
    BitLSMTestBase::SetUp();
    cache_ = rocksdb::NewLRUCache(64 << 20);
    table_options_.block_cache = cache_;
    table_options_.cache_index_and_filter_blocks = true;
  }

  // Writes kRows rows and flushes them into a single SST carrying a SABI
  // block.
  BitLSM& OpenAndFill() {
    BitLSM& db = OpenDB(TwoAttrOptions());
    for (int i = 0; i < kRows; ++i) {
      char key[16];
      std::snprintf(key, sizeof(key), "k%08d", i);
      std::vector<Attr> attrs = {std::string("c") + std::to_string(i % 10),
                                 static_cast<double>(i % 1000)};
      EXPECT_TRUE(db.Put(key, attrs, "payload-" + std::to_string(i)).ok());
    }
    EXPECT_TRUE(db.Flush().ok());
    return db;
  }

  static size_t RunQuery(BitLSM& db) {
    BitLSMQuery q = ConjunctiveQuery();
    auto it = db.NewIterator(q);
    EXPECT_NE(it, nullptr);
    size_t n = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) n++;
    // A complete scan: the iterator stopped because it ran out of rows, not
    // because a SABI load failed somewhere below it.
    EXPECT_TRUE(it->status().ok()) << it->status().ToString();
    return n;
  }

  std::shared_ptr<rocksdb::Cache> cache_;
};

// Workload: 10k rows flushed to an SST, then a conjunctive query, then a full
//           cache eviction, then the same query again.
// Threat: the SABI block is pinned outside the block cache (never charged,
//         never evictable), or eviction loses it permanently and the second
//         query silently returns fewer rows.
TEST_F(UDIBlockCacheTest, SABIIsChargedToCacheAndSurvivesEviction) {
  BitLSM& db = OpenAndFill();

  // Start from a cold cache: the flush's table open already warmed the SABI
  // block, and that must not be mistaken for the query's own charge.
  cache_->EraseUnRefEntries();
  const size_t usage_cold = cache_->GetUsage();

  ASSERT_EQ(RunQuery(db), kExpectedMatches);
  // The SABI entry (raw block + parsed reader) is charged to the cache.
  const size_t usage_warm = cache_->GetUsage();
  ASSERT_GT(usage_warm, usage_cold);

  // Evict everything unpinned: the query's iterator is gone, so nothing holds
  // the SABI entry any more and it must actually leave the cache.
  cache_->EraseUnRefEntries();
  ASSERT_LT(cache_->GetUsage(), usage_warm);

  // Re-query: the SABI block must be re-read from the SST and re-parsed,
  // producing identical results.
  ASSERT_EQ(RunQuery(db), kExpectedMatches);
}

// Workload: a full cache eviction fired mid-scan, and another one against a
//           still-live (but exhausted) iterator.
// Threat: the iterator borrows bitmaps and block handles from a reader it
//         does not keep pinned, so eviction frees the reader under a live
//         scan. Note the eviction is asserted through the cache's own
//         accounting, not through the scan's output: a use-after-free reads
//         intact freed memory often enough that result-only assertions pass
//         in a release build whether the pin exists or not.
TEST_F(UDIBlockCacheTest, LiveIteratorKeepsSABIPinnedAgainstEviction) {
  BitLSM& db = OpenAndFill();

  // Baseline with the table already open and nothing of ours pinned: a first
  // query opens the SST's table reader, whose own cache entries must not be
  // mistaken for the iterator's pin.
  ASSERT_EQ(RunQuery(db), kExpectedMatches);
  cache_->EraseUnRefEntries();
  const size_t usage_evicted = cache_->GetUsage();

  BitLSMQuery q = ConjunctiveQuery();
  auto it = db.NewIterator(q);
  ASSERT_NE(it, nullptr);

  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    // Drop everything unpinned once the scan is under way. The iterator's own
    // pin must keep the SABI entry (and the reader it borrows from) alive for
    // the rest of the scan.
    if (n == kExpectedMatches / 2) cache_->EraseUnRefEntries();
    n++;
  }
  EXPECT_EQ(n, kExpectedMatches);
  it.reset();

  // Now the pin itself, isolated: this query matches nothing, so the iterator
  // never loads (and never pins) a data block. Anything a full eviction still
  // cannot reclaim while it is alive is the SABI entry it holds.
  cache_->EraseUnRefEntries();
  ASSERT_EQ(cache_->GetUsage(), usage_evicted);

  BitLSMQuery empty_q = NoMatchQuery();
  auto empty_it = db.NewIterator(empty_q);
  ASSERT_NE(empty_it, nullptr);
  empty_it->SeekToFirst();
  ASSERT_FALSE(empty_it->Valid());
  // Invalid because nothing matched, which must stay distinguishable from
  // invalid because the SABI block could not be loaded.
  EXPECT_TRUE(empty_it->status().ok()) << empty_it->status().ToString();

  cache_->EraseUnRefEntries();
  EXPECT_GT(cache_->GetUsage(), usage_evicted);

  // Destroying the iterator releases that pin — the entry must become
  // reclaimable again, or the pin leaks for the process's lifetime.
  empty_it.reset();
  cache_->EraseUnRefEntries();
  EXPECT_EQ(cache_->GetUsage(), usage_evicted);
}

// Workload: an SST written by a plain RocksDB (no user-defined index factory),
//           then reopened through BitLSM and queried.
// Threat: the file carries no SABI block, so the table iterator cannot produce
//         a single one of its rows -- and an iterator that reports that as a
//         plain !Valid() is indistinguishable from "nothing matched", turning
//         an unreadable file into a silently incomplete query result.
TEST_F(UDIBlockCacheTest, SABILessSSTFailsTheQueryInsteadOfSkippingRows) {
  const BitLSMOptions o = TwoAttrOptions();

  // 1. Write the SST from a plain RocksDB: the default table factory emits no
  // user-defined index block. Values are still BitLSM-encoded, so the query
  // has to fail on the missing index rather than on a decode.
  {
    // Inherits the fixture's env (MEM_ENV) and create_if_missing.
    rocksdb::Options plain_options = rocksdb_options_;
    plain_options.table_factory.reset(
        rocksdb::NewBlockBasedTableFactory(rocksdb::BlockBasedTableOptions()));
    rocksdb::DB* raw_db = nullptr;
    BITLSM_ASSERT_OK(rocksdb::DB::Open(plain_options, db_path_, &raw_db));
    std::unique_ptr<rocksdb::DB> plain_db(raw_db);
    for (int i = 0; i < 16; ++i) {
      char key[16];
      std::snprintf(key, sizeof(key), "k%08d", i);
      std::string value;
      EncodeValue(o,
                  {std::string("c") + std::to_string(i % 10),
                   static_cast<double>(i % 1000)},
                  "payload", value);
      BITLSM_ASSERT_OK(plain_db->Put(rocksdb::WriteOptions(), key, value));
    }
    BITLSM_ASSERT_OK(plain_db->Flush(rocksdb::FlushOptions()));
    BITLSM_ASSERT_OK(plain_db->Close());
  }

  // 2. Reopen the same path through BitLSM. fail_if_no_udi_on_open defaults to
  // false, so the open succeeds with a warning and the failure can only
  // surface at query time.
  BitLSM& db = OpenDB(o);

  BitLSMQuery q = ConjunctiveQuery();
  auto it = db.NewIterator(q);
  ASSERT_NE(it, nullptr);
  it->SeekToFirst();

  // 3. The scan stops, and says why: NotFound is what the table reader returns
  // for a file with no user-defined index.
  EXPECT_FALSE(it->Valid());
  EXPECT_FALSE(it->status().ok());
  EXPECT_TRUE(it->status().IsNotFound()) << it->status().ToString();
}

// Workload: two parsed SABIReaders over indexes of very different size.
// Threat: ApproximateMemoryUsage stays the 0 stub (or any constant), so the
//         block cache charges only the raw block bytes and the parsed
//         reader's heap is invisible to the cache's capacity accounting.
//
// Asserted here rather than through cache_->GetUsage(): a query also charges
// the SST's data and index blocks, which on their own clear any plausible
// floor — a cache-level assertion passes with the 0 stub still in place.
TEST(SABIReaderMemoryUsage, TracksIndexContent) {
  BuiltIndex small = BuildIndex(/*rows=*/64, /*distinct=*/4);
  BuiltIndex big = BuildIndex(/*rows=*/20000, /*distinct=*/2000);

  // Even the small index owns heap beyond the reader object itself (the
  // frozen bitmap buffers), so a 0 return is impossible.
  EXPECT_GT(small.reader->ApproximateMemoryUsage(), sizeof(SABIReader));

  // ... and the accounting has to move with the content: the big index
  // interns 2000 non-SSO values in its binning policy alone.
  EXPECT_GT(big.reader->ApproximateMemoryUsage(),
            small.reader->ApproximateMemoryUsage() + 4096);
}
