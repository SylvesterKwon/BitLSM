#pragma once

#include "bit_lsm.h"
#include "test_util/reference_db.h"
#include <cstdint>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

namespace bit_lsm {

// doublechecked wrapper: applies each mutation to the real engine AND the
// reference oracle, then verifies query/scan equivalence. Verification methods
// return a gtest AssertionResult carrying a seed/op-trace/diff message, so the
// caller picks ASSERT_TRUE (stop) or EXPECT_TRUE (continue).
class CheckedBitLSM {
 public:
  CheckedBitLSM(BitLSM* engine, BitLSMOptions options)
      : engine_(engine), options_(std::move(options)), ref_(options_) {}

  // Mutations: applied to both sides + appended to the trace.
  ::testing::AssertionResult Put(const std::string& key,
                                 const std::vector<Attr>& attrs,
                                 const std::string& payload);
  ::testing::AssertionResult Delete(const std::string& key);
  ::testing::AssertionResult PutBatch(
      const std::vector<std::string>& keys,
      const std::vector<std::vector<Attr>>& attrs_list,
      const std::vector<std::string>& payloads);

  // LSM shape changes: engine only; logical state unchanged.
  ::testing::AssertionResult Flush();
  ::testing::AssertionResult CompactAll();

  // The core differential checks.
  ::testing::AssertionResult VerifyQuery(BitLSMQuery query);
  ::testing::AssertionResult VerifyFullScan();  // empty query == all live

  // Direct access for meta-tests / advanced drivers.
  ReferenceDB& reference() { return ref_; }
  void set_seed(std::uint64_t seed) { seed_ = seed; has_seed_ = true; }

 private:
  std::map<std::string, Record> ScanEngine(BitLSMQuery& query);
  std::string Context() const;  // seed + op trace for failure messages

  BitLSM* engine_;
  BitLSMOptions options_;
  ReferenceDB ref_;
  std::vector<std::string> trace_;
  std::uint64_t seed_ = 0;
  bool has_seed_ = false;
};

}  // namespace bit_lsm
