#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "test_util/reference_db.h"

namespace bit_lsm {

// All randomness flows through an injected RNG — never global rand() — so a
// single 64-bit seed reproduces an entire run (BITLSM_TEST_SEED).
// Stream reproducibility is per standard-library implementation
// (std::uniform_*_distribution algorithms are implementation-defined);
// CI and dev both use libstdc++.
using Rng = std::mt19937_64;

// Random BitLSMOptions: attr_num 1-5, random type mix, rho in
// {0.5, 0.2, 0.05} (coarse rho = bin-collision pressure, fine = many bins).
BitLSMOptions GenerateSchema(Rng& rng);

struct WorkloadParams {
  uint32_t key_pool = 100;  // distinct keys -> overwrite/delete collisions
  uint32_t categorical_dict = 8;  // small dict; can exceed bin budget
};

enum class OpKind { kPut, kDelete, kPutBatch, kFlush, kCompactAll };

// Weighted op mix: Put 60%, Delete 15%, PutBatch 10%, Flush 10%, Compact 5%.
OpKind PickOp(Rng& rng);

// Biased key draw (hot keys) from the pool: "k0".."k<pool-1>".
std::string GenerateKey(Rng& rng, const WorkloadParams& p);

// Attr vector for a Put. Continuous values mix: exact repeats of stored
// values (EQUAL hits), near-stored offsets (bin-boundary pressure), uniform.
std::vector<Attr> GenerateAttrs(Rng& rng, const BitLSMOptions& schema,
                                const WorkloadParams& p,
                                const ReferenceDB& oracle);

// Random full-CNF query: 0-3 clauses (0 = empty query -> full scan), 1-3
// conditions each, attr drawn independently per condition (cross-attr and
// mixed-type clauses arise naturally). Categorical conditions are EQUAL-only
// (engine contract). Always Validate()-clean by construction.
BitLSMQuery GenerateQuery(Rng& rng, const BitLSMOptions& schema,
                          const WorkloadParams& p, const ReferenceDB& oracle);

}  // namespace bit_lsm
