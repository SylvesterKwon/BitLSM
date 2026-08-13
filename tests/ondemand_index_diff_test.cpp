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

// Workload: the randomized differential stream (Put/Delete/PutBatch/Flush/
//           Compact plus periodic CNF query batteries against the oracle),
//           run with BitLSMOptions::ondemand_index on.
// Threat: the on-demand read path decodes the blob a second way -- bin extents
//         from the directory, bitmaps read per query and frozen-viewed over
//         freshly aligned buffers -- so any drift from the resident reader
//         shows up as missing or extra rows. Compaction churn also exercises
//         the registry's file-deletion hook, since a stale directory would
//         answer with another file's bin offsets.
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
