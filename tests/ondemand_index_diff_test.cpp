#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

#include "test_util/bitlsm_test_base.h"
#include "test_util/diff_driver.h"

using namespace bit_lsm;

namespace {

class OnDemandIndexDiffTest
    : public BitLSMTestBase,
      public ::testing::WithParamInterface<std::uint64_t> {};

}  // namespace

// The randomized differential run with ondemand_index on. Compaction
// churn also exercises reader teardown.
TEST_P(OnDemandIndexDiffTest, EngineMatchesOracle) {
  std::uint64_t seed = EffectiveSeed(GetParam());
  std::cerr << "BITLSM_TEST_SEED=" << seed << "\n";
  Rng rng(seed);
  BitLSMOptions schema = GenerateSchema(rng);
  schema.ondemand_index = true;
  CheckedBitLSM db(&OpenDB(schema), schema);
  db.set_seed(seed);
  DiffParams params;  // fast tier defaults: 400 steps, verify every 25
  params.workload.key_pool =
      std::uniform_int_distribution<std::uint32_t>(50, 200)(rng);
  RunRandomizedDiff(db, rng, schema, params);
}

INSTANTIATE_TEST_SUITE_P(FastTier, OnDemandIndexDiffTest,
                         ::testing::Values<std::uint64_t>(1, 2, 3, 4));
