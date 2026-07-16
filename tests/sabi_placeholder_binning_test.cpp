#include <gtest/gtest.h>
#include <rocksdb/slice.h>
#include <rocksdb/user_defined_index.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "sabi.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

// One ORDERED int64 attr + one UNORDERED attr; rho 0.25 -> bin budget 8.
BitLSMOptions MixedOptions() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {
      AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true, /*is_float=*/false,
               /*nullable=*/true),
      AttrSpec(AttrRole::UNORDERED)};
  o.read_seqno = 0;
  o.rho = 0.25;
  return o;
}

struct BuiltIndex {
  std::unique_ptr<SABIBuilder> builder;
  std::unique_ptr<SABIReader> reader;
};

// data_rows Put rows {i, "c<i%4>"}, then null_rows Puts with a NULL ordered
// attr, then tombstone_rows deletion entries.
BuiltIndex BuildMixed(uint32_t data_rows, uint32_t null_rows,
                      uint32_t tombstone_rows) {
  BitLSMOptions options = MixedOptions();
  BuiltIndex built;
  built.builder = std::make_unique<SABIBuilder>(
      SABISchema::FromOptions(options),
      std::make_unique<ValueLayoutExtractor>(options));

  std::vector<std::string> keys, encoded;
  uint32_t total = data_rows + null_rows + tombstone_rows;
  keys.reserve(total);
  encoded.reserve(data_rows + null_rows);
  for (uint32_t i = 0; i < data_rows; ++i) {
    keys.push_back("k" + std::to_string(i));
    std::string out;
    EncodeValue(options, {int64_t(i), std::string("c") + std::to_string(i % 4)},
                "p", out);
    encoded.push_back(std::move(out));
    built.builder->OnKeyAdded(rocksdb::Slice(keys.back()),
                              UDIB::ValueType::kValue,
                              rocksdb::Slice(encoded.back()));
  }
  for (uint32_t i = 0; i < null_rows; ++i) {
    keys.push_back("n" + std::to_string(i));
    std::string out;
    EncodeValue(options, {std::monostate{}, std::string("c0")}, "p", out);
    encoded.push_back(std::move(out));
    built.builder->OnKeyAdded(rocksdb::Slice(keys.back()),
                              UDIB::ValueType::kValue,
                              rocksdb::Slice(encoded.back()));
  }
  for (uint32_t i = 0; i < tombstone_rows; ++i) {
    keys.push_back("t" + std::to_string(i));
    built.builder->OnKeyAdded(rocksdb::Slice(keys.back()),
                              UDIB::ValueType::kTypeDeletion, rocksdb::Slice());
  }

  std::string scratch;
  UDIB::BlockHandle block_handle{/*offset=*/100, /*size=*/4096};
  built.builder->AddIndexEntry(rocksdb::Slice(keys.back()), nullptr,
                               block_handle, &scratch);
  rocksdb::Slice blob;
  EXPECT_TRUE(built.builder->Finish(&blob).ok());
  built.reader = std::make_unique<SABIReader>(blob);
  return built;
}

// Sum of the value-bin cardinalities of attr `i` in the parsed reader.
uint64_t BinCardinalitySum(const SABIReader& reader, uint32_t i) {
  uint32_t offset = 0;
  for (uint32_t a = 0; a < i; ++a) offset += reader.bitmap_index.bitmap_nums[a];
  uint64_t sum = 0;
  for (uint32_t b = 0; b < reader.bitmap_index.bitmap_nums[i]; ++b)
    sum += reader.bitmap_index.bitmaps[offset + b].cardinality();
  return sum;
}

}  // namespace

// Workload: 100 data rows, 10 NULL-attr rows, and 5 deletion entries in one
//           blob; inspect every value bin of the ORDERED attr.
// Threat: placeholder rows (NULL/tombstone) leaking into value bins — a
//         tombstone binned by its dummy okey 0 lands in the lowest bin and
//         bloats every range query touching it.
TEST(SabiPlaceholderBinning, OrderedValueBinsHoldOnlyDataRows) {
  BuiltIndex built = BuildMixed(/*data_rows=*/100, /*null_rows=*/10,
                                /*tombstone_rows=*/5);
  const SABIReader& reader = *built.reader;

  // Every data row is in exactly one bin; NULL and tombstone rows in none.
  EXPECT_EQ(BinCardinalitySum(reader, 0), 100u);

  // Boundaries stay pinned to the data bounds despite the placeholders.
  const auto& boundaries =
      std::get<std::vector<uint64_t>>(reader.bitmap_index.binning_policy[0]);
  EXPECT_EQ(boundaries.front(), I64ToOkey(0));
  EXPECT_EQ(boundaries.back(), I64ToOkey(99));
}

// Workload: the same blob; inspect the UNORDERED attr's binning policy and
//           bins. Only "c0".."c3" are real category bytes.
// Threat: NULL/tombstone placeholders interned as "" fabricate a phantom
//         category — it wastes a bin slot, skews the frequency-based bin
//         packing, and makes EQ "" unprunable on SSTs with no real "" data.
TEST(SabiPlaceholderBinning, UnorderedPolicyHasNoPhantomEmptyCategory) {
  BuiltIndex built = BuildMixed(/*data_rows=*/100, /*null_rows=*/10,
                                /*tombstone_rows=*/5);
  const SABIReader& reader = *built.reader;

  const auto& entries = std::get<std::vector<std::pair<std::string, uint32_t>>>(
      reader.bitmap_index.binning_policy[1]);
  ASSERT_EQ(entries.size(), 4u);  // c0..c3 only, no ""
  for (const auto& e : entries) EXPECT_NE(e.first, "");

  // NULL rows carry a real "c0" here, so only the 5 tombstones are excluded.
  EXPECT_EQ(BinCardinalitySum(reader, 1), 110u);
}
