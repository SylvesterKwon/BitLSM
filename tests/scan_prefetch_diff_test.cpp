#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

#include "block_prefetch_queue.h"
#include "test_util/bitlsm_test_base.h"
#include "test_util/diff_driver.h"

using namespace bit_lsm;

namespace {

class ScanPrefetchDiffTest
    : public BitLSMTestBase,
      public ::testing::WithParamInterface<std::uint64_t> {};

}  // namespace

// The randomized differential run with scan_prefetch_depth set; a slot
// handed to the wrong block shows up as wrong or missing rows.
TEST_P(ScanPrefetchDiffTest, EngineMatchesOracle) {
  std::uint64_t seed = EffectiveSeed(GetParam());
  std::cerr << "BITLSM_TEST_SEED=" << seed << "\n";
  Rng rng(seed);
  BitLSMOptions schema = GenerateSchema(rng);
  schema.scan_prefetch_depth = 8;
  ResetBlockPrefetchQueueStats();
  CheckedBitLSM db(&OpenDB(schema), schema);
  db.set_seed(seed);
  DiffParams params;  // fast tier defaults: 400 steps, verify every 25
  params.workload.key_pool =
      std::uniform_int_distribution<std::uint32_t>(50, 200)(rng);
  RunRandomizedDiff(db, rng, schema, params);

  // Guard against passing because the queue never ran, which would make
  // this a test of the synchronous path.
  const BlockPrefetchQueueStats stats = GetBlockPrefetchQueueStats();
  std::cerr << "prefetch queue: " << stats.served << " blocks served\n";
  if (stats.async_unavailable) {
    // Every block fell back to a synchronous read. The oracle comparison above
    // still ran; the CI image has no liburing either.
    GTEST_SKIP() << "no io_uring in this build; queue cannot submit";
  }
  EXPECT_GT(stats.served, 0u);
}

INSTANTIATE_TEST_SUITE_P(FastTier, ScanPrefetchDiffTest,
                         ::testing::Values<std::uint64_t>(1, 2, 3, 4));
