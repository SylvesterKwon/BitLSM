#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

#include "test_util/bitlsm_test_base.h"
#include "test_util/diff_driver.h"

using namespace bit_lsm;

namespace {

class RandomizedDiffTest : public BitLSMTestBase,
                           public ::testing::WithParamInterface<std::uint64_t> {
};

}  // namespace

// Workload: seeded random op stream (Put/Delete/PutBatch/Flush/Compact) on a
//           random schema and a seed-varied key pool (50-200), with periodic
//           random full-CNF query batteries differentially checked against
//           the oracle.
// Threat: any engine/oracle divergence in write semantics, binning, CNF
//         evaluation, tombstones, or compaction that the hand-written
//         scenarios did not anticipate.
TEST_P(RandomizedDiffTest, EngineMatchesOracle) {
  std::uint64_t seed = EffectiveSeed(GetParam());
  // Echo the seed up front so even exception/crash failures (which bypass
  // CheckedBitLSM's repro dump) identify their seed.
  std::cerr << "BITLSM_TEST_SEED=" << seed << "\n";
  Rng rng(seed);
  BitLSMOptions schema = GenerateSchema(rng);
  CheckedBitLSM db(&OpenDB(schema), schema);
  db.set_seed(seed);
  DiffParams params;  // fast tier defaults: 400 steps, verify every 25
  params.workload.key_pool =
      std::uniform_int_distribution<std::uint32_t>(50, 200)(rng);
  RunRandomizedDiff(db, rng, schema, params);
}

INSTANTIATE_TEST_SUITE_P(FastTier, RandomizedDiffTest,
                         ::testing::Values<std::uint64_t>(1, 2, 3, 4));
