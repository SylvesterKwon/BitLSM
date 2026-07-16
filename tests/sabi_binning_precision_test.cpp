#include <gtest/gtest.h>
#include <rocksdb/slice.h>
#include <rocksdb/user_defined_index.h>

#include <cstdint>
#include <memory>
#include <set>
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

// Single ORDERED int64 attr; rho 0.1 -> bin budget 10.
BitLSMOptions I64Options() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {
      AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true, /*is_float=*/false)};
  o.read_seqno = 0;
  o.rho = 0.1;
  return o;
}

// Builder + reader over one blob; the builder owns the blob memory, so it
// must stay alive at least as long as the reader is being constructed.
struct BuiltIndex {
  std::unique_ptr<SABIBuilder> builder;
  std::unique_ptr<SABIReader> reader;
};

// Feeds `values` as Put rows into a SABIBuilder and parses the finished
// blob.
BuiltIndex BuildI64Index(const std::vector<int64_t>& values) {
  BitLSMOptions options = I64Options();
  BuiltIndex built;
  built.builder = std::make_unique<SABIBuilder>(
      SABISchema::FromOptions(options),
      std::make_unique<ValueLayoutExtractor>(options));

  std::vector<std::string> keys, encoded;
  keys.reserve(values.size());
  encoded.reserve(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    keys.push_back("k" + std::to_string(i));
    std::string out;
    EncodeValue(options, {values[i]}, "p", out);
    encoded.push_back(std::move(out));
    built.builder->OnKeyAdded(rocksdb::Slice(keys[i]), UDIB::ValueType::kValue,
                              rocksdb::Slice(encoded[i]));
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

// The binning quality checks shared by both regression cases: boundaries
// must partition the okey span (not collapse to [min, min, ..., max]) and no
// single bin may absorb every row.
void ExpectBinsActuallyPartition(const SABIReader& reader, size_t row_cnt) {
  uint32_t bins = reader.bitmap_index.bitmap_nums[0];
  ASSERT_GT(bins, 1u) << "budget must allocate >1 bin for this workload";

  const auto& boundaries =
      std::get<std::vector<uint64_t>>(reader.bitmap_index.binning_policy[0]);
  ASSERT_EQ(boundaries.size(), bins + 1);
  std::set<uint64_t> distinct(boundaries.begin(), boundaries.end());
  EXPECT_GE(distinct.size(), bins / 2 + 2)
      << "interior boundaries collapsed to a single okey";

  uint64_t max_card = 0;
  for (uint32_t b = 0; b < bins; ++b)
    max_card = std::max(max_card, reader.bitmap_index.bitmaps[b].cardinality());
  EXPECT_LT(max_card, row_cnt / 2)
      << "one bin holds (almost) every row; range pruning is impossible";
}

}  // namespace

// Workload: 1000 int64 values 0..999 (okeys 2^63..2^63+999, span 999 < the
//           double ULP of 2048 near 2^63), single blob, bin budget 10.
// Threat: projecting absolute okeys to double collapses the whole span onto
//         one representable double, so every quantile boundary is equal and
//         all rows fold into a single bin — range/equality pruning silently
//         does nothing for narrow-span attributes (age, enum codes, ...).
TEST(SabiBinningPrecision, NarrowInt64SpanSpreadsAcrossBins) {
  std::vector<int64_t> values;
  values.reserve(1000);
  for (int64_t i = 0; i < 1000; ++i) values.push_back(i);

  BuiltIndex built = BuildI64Index(values);
  const SABIReader& reader = *built.reader;

  const auto& boundaries =
      std::get<std::vector<uint64_t>>(reader.bitmap_index.binning_policy[0]);
  // Outer thresholds stay pinned to the exact data bounds.
  EXPECT_EQ(boundaries.front(), I64ToOkey(0));
  EXPECT_EQ(boundaries.back(), I64ToOkey(999));

  ExpectBinsActuallyPartition(reader, values.size());
}
