#pragma once

#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_util/checked_bitlsm.h"
#include "test_util/generators.h"

namespace bit_lsm {

struct DiffParams {
  uint32_t steps = 400;            // random ops per run
  uint32_t verify_every = 25;      // ops between query batteries
  uint32_t queries_per_check = 4;  // random queries per battery
  uint32_t final_queries = 16;     // battery after the last op
  WorkloadParams workload;
};

// Effective seed for a parameterized run: BITLSM_TEST_SEED overrides the
// parameter, so a reported seed reproduces under any instantiation (run a
// single one via ctest -R). Non-numeric input fails loudly instead of
// silently becoming seed 0.
// NOTE: ctest test names show parameter VALUES (e.g. .../1000 = seed), but a
// raw gtest binary's --gtest_filter uses INDICES — when bypassing ctest,
// always pin the seed via BITLSM_TEST_SEED instead of the filter number.
inline std::uint64_t EffectiveSeed(std::uint64_t param_seed) {
  const char* env = std::getenv("BITLSM_TEST_SEED");
  if (!env) return param_seed;
  char* end = nullptr;
  std::uint64_t seed = std::strtoull(env, &end, 10);
  EXPECT_TRUE(end != env && *end == '\0')
      << "BITLSM_TEST_SEED is not a number: " << env;
  return seed;
}

// One randomized differential run: `steps` random ops against the checked
// wrapper with periodic VerifyQuery/VerifyFullScan batteries. ASSERTs stop at
// the first divergence; the failure message carries seed + op trace + diff
// (CheckedBitLSM::Context), reproducible via BITLSM_TEST_SEED=<seed>.
// Call as the last statement of the test body (or wrap in
// ASSERT_NO_FATAL_FAILURE): a fatal failure aborts only this helper.
inline void RunRandomizedDiff(CheckedBitLSM& db, Rng& rng,
                              const BitLSMOptions& schema,
                              const DiffParams& p) {
  assert(p.verify_every > 0);  // used as a modulus below
  for (uint32_t step = 1; step <= p.steps; ++step) {
    switch (PickOp(rng)) {
      case OpKind::kPut: {
        // Sequenced locals: argument evaluation order is unspecified, and
        // both calls consume RNG draws — order must not depend on compiler.
        std::string key = GenerateKey(rng, p.workload);
        std::vector<Attr> attrs =
            GenerateAttrs(rng, schema, p.workload, db.reference());
        ASSERT_TRUE(db.Put(key, attrs, "p" + std::to_string(step)));
        break;
      }
      case OpKind::kDelete:
        ASSERT_TRUE(db.Delete(GenerateKey(rng, p.workload)));
        break;
      case OpKind::kPutBatch: {
        uint32_t n = std::uniform_int_distribution<uint32_t>(2, 8)(rng);
        std::vector<std::string> keys;
        std::vector<std::vector<Attr>> attrs;
        std::vector<std::string> payloads;
        for (uint32_t i = 0; i < n; ++i) {
          keys.push_back(GenerateKey(rng, p.workload));  // dups possible
          attrs.push_back(
              GenerateAttrs(rng, schema, p.workload, db.reference()));
          payloads.push_back("b" + std::to_string(step) + "_" +
                             std::to_string(i));
        }
        ASSERT_TRUE(db.PutBatch(keys, attrs, payloads));
        break;
      }
      case OpKind::kFlush:
        ASSERT_TRUE(db.Flush());
        break;
      case OpKind::kCompactAll:
        ASSERT_TRUE(db.CompactAll());
        break;
    }
    if (step % p.verify_every == 0) {
      for (uint32_t q = 0; q < p.queries_per_check; ++q)
        ASSERT_TRUE(db.VerifyQuery(
            GenerateQuery(rng, schema, p.workload, db.reference())));
      ASSERT_TRUE(db.VerifyFullScan());
    }
  }
  for (uint32_t q = 0; q < p.final_queries; ++q)
    ASSERT_TRUE(
        db.VerifyQuery(GenerateQuery(rng, schema, p.workload, db.reference())));
  ASSERT_TRUE(db.VerifyFullScan());
}

}  // namespace bit_lsm
