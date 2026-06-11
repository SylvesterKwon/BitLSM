#include "test_util/generators.h"

#include <gtest/gtest.h>

#include <string>

using namespace bit_lsm;

// Workload: generate 100 seeded schemas.
// Threat: generator emits schemas violating BitLSMOptions invariants
//         (attr_num/attr_types mismatch, rho<=0) -> engine UB downstream.
TEST(Generators, SchemasAreWellFormed) {
  Rng rng(42);
  for (int i = 0; i < 100; ++i) {
    BitLSMOptions s = GenerateSchema(rng);
    EXPECT_GE(s.attr_num, 1u);
    EXPECT_LE(s.attr_num, 5u);
    EXPECT_EQ(s.attr_types.size(), s.attr_num);
    EXPECT_GT(s.rho, 0.0);
  }
}

// Workload: 50 schemas x 50 queries each, over an oracle pre-seeded with
//           records so stored-value sampling kicks in.
// Threat: generator emits queries Validate() rejects -> the random driver
//         would feed nullptr iterators instead of testing engine semantics.
TEST(Generators, QueriesAlwaysValidate) {
  Rng rng(7);
  WorkloadParams p;
  for (int i = 0; i < 50; ++i) {
    BitLSMOptions s = GenerateSchema(rng);
    ReferenceDB oracle(s);
    for (int j = 0; j < 20; ++j)
      oracle.Put("k" + std::to_string(j), GenerateAttrs(rng, s, p, oracle),
                 "p");
    for (int q = 0; q < 50; ++q) {
      BitLSMQuery query = GenerateQuery(rng, s, p, oracle);
      rocksdb::Status st = query.Validate(s);
      EXPECT_TRUE(st.ok()) << query.ToString() << " : " << st.ToString();
    }
  }
}

// Workload: two RNGs with the same seed, drained identically.
// Threat: hidden nondeterminism (global rand, iteration-order dependence)
//         breaks seed-based failure reproduction.
TEST(Generators, SameSeedSameStream) {
  Rng a(123), b(123);
  BitLSMOptions sa = GenerateSchema(a);
  BitLSMOptions sb = GenerateSchema(b);
  EXPECT_EQ(sa.attr_num, sb.attr_num);
  EXPECT_EQ(sa.attr_types, sb.attr_types);
  ReferenceDB oa(sa), ob(sb);
  WorkloadParams p;
  Rng c(77), d(77);
  for (int j = 0; j < 5; ++j) {
    oa.Put("k" + std::to_string(j), GenerateAttrs(c, sa, p, oa), "p");
    ob.Put("k" + std::to_string(j), GenerateAttrs(d, sb, p, ob), "p");
  }
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(GenerateKey(a, p), GenerateKey(b, p));
    EXPECT_EQ(GenerateAttrs(a, sa, p, oa), GenerateAttrs(b, sb, p, ob));
    EXPECT_EQ(GenerateQuery(a, sa, p, oa).ToString(),
              GenerateQuery(b, sb, p, ob).ToString());
  }
}
