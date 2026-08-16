#include <gtest/gtest.h>

#include "test_util/bitlsm_test_base.h"
#include "test_util/checked_bitlsm.h"
#include "test_util/generators.h"

using namespace bit_lsm;

// Threat: the point of the redesign is that a bigger budget buys fewer
// decodes. If the decoded-bin cache is bypassed (as PR #45's raw-page cache
// was), the second identical query re-loads every bin and this fails.
TEST_F(BitLSMTestBase, SecondQueryServesBinsFromCache) {
  // Seed 1 verified to draw a non-empty-clause query (below) for both q1 and
  // q2: the query touches real bins, so bitmaps_loaded > 0 cold is meant to
  // be deterministic once the scan path is wired to Bin() (Task 7).
  Rng rng(1);
  BitLSMOptions schema = GenerateSchema(rng);
  schema.ondemand_index = true;
  CheckedBitLSM db(&OpenDB(schema), schema);
  WorkloadParams workload;  // defaults; same shape diff_driver uses
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db.Put("k" + std::to_string(i),
                       GenerateAttrs(rng, schema, workload, db.reference()),
                       "p" + std::to_string(i)));
  }
  ASSERT_TRUE(db.Flush());

  // Two byte-identical queries: generate from two copies of one rng state.
  Rng q1 = rng;
  Rng q2 = rng;
  ResetSABIBinCacheStats();
  ASSERT_TRUE(
      db.VerifyQuery(GenerateQuery(q1, schema, workload, db.reference())));
  SABIBinCacheStats first = GetSABIBinCacheStats();
  ASSERT_GT(first.bitmaps_loaded, 0u);  // cold: bins were read+decoded

  ASSERT_TRUE(
      db.VerifyQuery(GenerateQuery(q2, schema, workload, db.reference())));
  SABIBinCacheStats second = GetSABIBinCacheStats();
  EXPECT_EQ(second.bitmaps_loaded, first.bitmaps_loaded)
      << "warm query decoded bins again: the budget buys nothing";
  EXPECT_GT(second.hits, first.hits);
}
