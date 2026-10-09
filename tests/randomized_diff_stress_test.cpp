#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

#include "test_util/bitlsm_test_base.h"
#include "test_util/diff_driver.h"

using namespace bit_lsm;

namespace {

class RandomizedDiffStressTest
    : public BitLSMTestBase,
      public ::testing::WithParamInterface<std::uint64_t> {};

}  // namespace

// The fast-tier run with a bigger budget (32 seeds x 5000 steps).
// Labelled "stress": excluded from PR CI, run locally with ctest -L
// stress.
TEST_P(RandomizedDiffStressTest, EngineMatchesOracle) {
  std::uint64_t seed = EffectiveSeed(GetParam());
  // Echo the seed up front so even exception/crash failures (which bypass
  // CheckedBitLSM's repro dump) identify their seed.
  std::cerr << "BITLSM_TEST_SEED=" << seed << "\n";
  Rng rng(seed);
  BitLSMOptions schema = GenerateSchema(rng);
  CheckedBitLSM db(&OpenDB(schema), schema);
  db.set_seed(seed);
  DiffParams params;
  params.steps = 5000;
  params.verify_every = 100;
  params.final_queries = 32;
  params.workload.key_pool =
      std::uniform_int_distribution<std::uint32_t>(50, 200)(rng);
  RunRandomizedDiff(db, rng, schema, params);
}

INSTANTIATE_TEST_SUITE_P(StressTier, RandomizedDiffStressTest,
                         ::testing::Range<std::uint64_t>(1000, 1032));
