#include <gtest/gtest.h>
#include <rocksdb/slice.h>
#include <rocksdb/user_defined_index.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "sabi.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

// Single ORDERED int64 attr; rho 0.1 -> bin budget 10. NULL rows only exist
// for nullable attrs (non-nullable NULLs encode as a placeholder value).
BitLSMOptions I64Options(bool nullable = false) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/false, nullable)};
  o.read_seqno = 0;
  o.rho = 0.1;
  return o;
}

// {UNORDERED, ORDERED i64, ORDERED i64}: the leading unordered attr shifts
// the ordered attrs' bitmap ranges inside the flat bitmap array.
BitLSMOptions MixedOptions() {
  BitLSMOptions o;
  o.attr_num = 3;
  o.attr_specs = {AttrSpec{AttrRole::UNORDERED},
                  AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/false),
                  AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/false)};
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

// Feeds `rows` as Put rows (plus `tombstone_cnt` deletion entries) into a
// SABIBuilder and parses the finished blob.
BuiltIndex BuildIndex(const BitLSMOptions& options,
                      const std::vector<std::vector<Attr>>& rows,
                      uint32_t tombstone_cnt = 0) {
  BuiltIndex built;
  built.builder = std::make_unique<SABIBuilder>(
      SABISchema::FromOptions(options),
      std::make_unique<ValueLayoutExtractor>(options));

  std::vector<std::string> keys, encoded;
  keys.reserve(rows.size() + tombstone_cnt);
  encoded.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    keys.push_back("k" + std::to_string(i));
    std::string out;
    EncodeValue(options, rows[i], "p", out);
    encoded.push_back(std::move(out));
    built.builder->OnKeyAdded(rocksdb::Slice(keys[i]), UDIB::ValueType::kValue,
                              rocksdb::Slice(encoded[i]));
  }
  for (uint32_t i = 0; i < tombstone_cnt; ++i) {
    keys.push_back("t" + std::to_string(i));
    built.builder->OnKeyAdded(rocksdb::Slice(keys[rows.size() + i]),
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

// Semantic ground truth for bin membership: bin i covers okeys in
// [boundaries[i], boundaries[i+1]), and the last bin also includes rows
// equal to the top boundary.
std::vector<uint64_t> CountPerBin(const std::vector<uint64_t>& boundaries,
                                  const std::vector<int64_t>& values) {
  size_t bins = boundaries.size() - 1;
  std::vector<uint64_t> counts(bins, 0);
  for (int64_t v : values) {
    uint64_t okey = I64ToOkey(v);
    size_t idx = std::upper_bound(boundaries.begin(), boundaries.end(), okey) -
                 boundaries.begin();
    size_t bin = (idx == 0) ? 0 : idx - 1;
    if (bin >= bins) bin = bins - 1;
    counts[bin]++;
  }
  return counts;
}

uint64_t Sum(const std::vector<uint64_t>& v) {
  uint64_t s = 0;
  for (uint64_t x : v) s += x;
  return s;
}

}  // namespace

// Workload: 1000 int64 rows 0..999 (a narrow okey span), single ORDERED
//           attr, one blob, bin budget 10; read the histogram back.
// Threat: the boundaries are persisted after (okey - min_okey) t-digest
//         normalization; an accessor that leaks normalized values instead of
//         absolute okeys, or misaligns counts with bins, poisons every
//         DB-level estimate built on top of it.
TEST(SabiHistogram, SingleAttrHistogramMatchesData) {
  std::vector<std::vector<Attr>> rows;
  std::vector<int64_t> values;
  for (int64_t i = 0; i < 1000; ++i) {
    rows.push_back({i});
    values.push_back(i);
  }
  BuiltIndex built = BuildIndex(I64Options(), rows);

  OrderedAttrHistogram h;
  ASSERT_TRUE(built.reader->OrderedHistogram(0, &h));
  ASSERT_GE(h.boundaries.size(), 2u);
  ASSERT_EQ(h.counts.size(), h.boundaries.size() - 1);
  ASSERT_GT(h.counts.size(), 1u) << "budget must allocate >1 bin";

  // Absolute okey coordinates, pinned to the exact data bounds.
  EXPECT_EQ(h.boundaries.front(), I64ToOkey(0));
  EXPECT_EQ(h.boundaries.back(), I64ToOkey(999));
  EXPECT_TRUE(std::is_sorted(h.boundaries.begin(), h.boundaries.end()));

  EXPECT_EQ(Sum(h.counts), values.size());
  EXPECT_EQ(h.counts, CountPerBin(h.boundaries, values));
}

// Workload: {UNORDERED, ORDERED, ORDERED} schema where the two ordered attrs
//           carry disjoint value ranges (0..499 vs 1000..1499).
// Threat: the per-attr bitmap ranges live in one flat array; an off-by-one in
//         the attr offset arithmetic reads a neighbor attr's bitmaps and
//         returns plausible-but-wrong counts.
TEST(SabiHistogram, MixedSchemaAttrsKeepTheirOwnCounts) {
  std::vector<std::vector<Attr>> rows;
  std::vector<int64_t> a1_values, a2_values;
  const std::vector<std::string> cats = {"red", "green", "blue"};
  for (int64_t i = 0; i < 500; ++i) {
    int64_t a1 = i, a2 = 1000 + i;
    rows.push_back({cats[i % cats.size()], a1, a2});
    a1_values.push_back(a1);
    a2_values.push_back(a2);
  }
  BuiltIndex built = BuildIndex(MixedOptions(), rows);

  OrderedAttrHistogram h1, h2;
  ASSERT_TRUE(built.reader->OrderedHistogram(1, &h1));
  ASSERT_TRUE(built.reader->OrderedHistogram(2, &h2));

  EXPECT_EQ(h1.boundaries.front(), I64ToOkey(0));
  EXPECT_EQ(h1.boundaries.back(), I64ToOkey(499));
  EXPECT_EQ(h2.boundaries.front(), I64ToOkey(1000));
  EXPECT_EQ(h2.boundaries.back(), I64ToOkey(1499));

  EXPECT_EQ(Sum(h1.counts), a1_values.size());
  EXPECT_EQ(Sum(h2.counts), a2_values.size());
  EXPECT_EQ(h1.counts, CountPerBin(h1.boundaries, a1_values));
  EXPECT_EQ(h2.counts, CountPerBin(h2.boundaries, a2_values));
}

// Workload: a caller sweeping every attr index of a mixed blob, plus one
//           index past the end.
// Threat: an UNORDERED attr holds a string binning policy in the variant;
//         reading it as okey boundaries (or indexing past attr_num) is
//         undefined behavior instead of a clean "no histogram" answer.
TEST(SabiHistogram, RejectsUnorderedAndOutOfRangeAttrs) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 100; ++i) rows.push_back({std::string("cat"), i, i});
  BuiltIndex built = BuildIndex(MixedOptions(), rows);

  OrderedAttrHistogram h;
  EXPECT_FALSE(built.reader->OrderedHistogram(0, &h));  // UNORDERED
  EXPECT_FALSE(built.reader->OrderedHistogram(3, &h));  // out of range
}

// Workload: 100 valued rows, 20 rows with a NULL attr, 5 tombstones, one
//           blob.
// Threat: NULL and tombstone rows sit in no value bin by construction; a
//         histogram that re-adds them (e.g. by counting rows instead of bin
//         bitmap cardinality) overstates the attr's row mass.
TEST(SabiHistogram, CountsOnlyBinnedDataRows) {
  std::vector<std::vector<Attr>> rows;
  std::vector<int64_t> values;
  for (int64_t i = 0; i < 100; ++i) {
    rows.push_back({i});
    values.push_back(i);
  }
  for (int64_t i = 0; i < 20; ++i) rows.push_back({std::monostate{}});
  BuiltIndex built =
      BuildIndex(I64Options(/*nullable=*/true), rows, /*tombstone_cnt=*/5);

  OrderedAttrHistogram h;
  ASSERT_TRUE(built.reader->OrderedHistogram(0, &h));
  EXPECT_EQ(Sum(h.counts), values.size());
  EXPECT_EQ(h.counts, CountPerBin(h.boundaries, values));
}

// Workload: an SST whose ORDERED attr is NULL on every row.
// Threat: with zero binned rows the stored boundaries come from an empty
//         t-digest and are meaningless; exposing them as a valid histogram
//         would inject garbage cells into a global aggregate.
TEST(SabiHistogram, AllNullAttrHasNoHistogram) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 50; ++i) rows.push_back({std::monostate{}});
  BuiltIndex built = BuildIndex(I64Options(/*nullable=*/true), rows);

  OrderedAttrHistogram h;
  EXPECT_FALSE(built.reader->OrderedHistogram(0, &h));
}

namespace {

// Single UNORDERED attr; rho 0.1 -> bin budget 10.
BitLSMOptions UnorderedOptions(bool nullable = false) {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(AttrRole::UNORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/true, nullable)};
  o.read_seqno = 0;
  o.rho = 0.1;
  return o;
}

// {ORDERED i64, UNORDERED}: the leading ordered attr shifts the unordered
// attr's bitmap range inside the flat bitmap array.
BitLSMOptions OrderedThenUnorderedOptions() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/false),
                  AttrSpec{AttrRole::UNORDERED}};
  o.read_seqno = 0;
  o.rho = 0.1;
  return o;
}

double SumCounts(const UnorderedAttrValueCounts& c) {
  double s = 0;
  for (const auto& [value, count] : c.value_counts) s += count;
  return s;
}

}  // namespace

// Workload: 160 rows over {ORDERED, UNORDERED} where the unordered attr has
//           3 distinct values with known counts (red 100 / green 50 /
//           blue 10) and the bin budget covers all of them.
// Threat: with one value per bin every count must be exact; a crossed
//         value->bin mapping or an attr offset bug that reads the ordered
//         neighbor's bitmaps attaches wrong counts to wrong values.
TEST(SabiValueCounts, ExactCountsWhenBudgetCoversValues) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 160; ++i) {
    std::string color = i < 100 ? "red" : (i < 150 ? "green" : "blue");
    rows.push_back({i, color});
  }
  BuiltIndex built = BuildIndex(OrderedThenUnorderedOptions(), rows);

  UnorderedAttrValueCounts c;
  ASSERT_TRUE(built.reader->UnorderedValueCounts(1, &c));
  // Entries come back sorted by value, one per interned distinct value.
  std::vector<std::pair<std::string, double>> expected = {
      {"blue", 10.0}, {"green", 50.0}, {"red", 100.0}};
  EXPECT_EQ(c.value_counts, expected);
}

// Workload: 20 distinct values x 10 rows each on a 10-bin budget, so every
//           bin holds exactly two values of equal mass.
// Threat: reporting the full bin cardinality for every value sharing a bin
//         double-counts the mass (sum 2x the row count), inflating every
//         equality selectivity built on top.
TEST(SabiValueCounts, SharedBinsSplitCardinalityUniformly) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t v = 0; v < 20; ++v) {
    std::string value =
        "v" + std::string(v < 10 ? "0" : "") + std::to_string(v);
    for (int64_t i = 0; i < 10; ++i) rows.push_back({value});
  }
  BuiltIndex built = BuildIndex(UnorderedOptions(), rows);

  UnorderedAttrValueCounts c;
  ASSERT_TRUE(built.reader->UnorderedValueCounts(0, &c));
  ASSERT_EQ(c.value_counts.size(), 20u);
  for (const auto& [value, count] : c.value_counts)
    EXPECT_DOUBLE_EQ(count, 10.0) << "value " << value;
  EXPECT_DOUBLE_EQ(SumCounts(c), 200.0);
}

// Workload: a caller sweeping every attr index of a mixed blob, plus one
//           index past the end.
// Threat: an ORDERED attr holds okey boundaries in the variant; reading it
//         as string entries (or indexing past attr_num) is undefined
//         behavior instead of a clean "no counts" answer.
TEST(SabiValueCounts, RejectsOrderedAndOutOfRangeAttrs) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 100; ++i) rows.push_back({i, std::string("cat")});
  BuiltIndex built = BuildIndex(OrderedThenUnorderedOptions(), rows);

  UnorderedAttrValueCounts c;
  EXPECT_FALSE(built.reader->UnorderedValueCounts(0, &c));  // ORDERED
  EXPECT_FALSE(built.reader->UnorderedValueCounts(2, &c));  // out of range
}

// Workload: 100 valued rows, 20 rows with a NULL attr, 5 tombstones, one
//           blob.
// Threat: NULL and tombstone rows sit in no value bin by construction; a
//         count that re-adds them overstates the attr's row mass.
TEST(SabiValueCounts, CountsOnlyBinnedDataRows) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 100; ++i)
    rows.push_back({std::string(i % 2 == 0 ? "even" : "odd")});
  for (int64_t i = 0; i < 20; ++i) rows.push_back({std::monostate{}});
  BuiltIndex built = BuildIndex(UnorderedOptions(/*nullable=*/true), rows,
                                /*tombstone_cnt=*/5);

  UnorderedAttrValueCounts c;
  ASSERT_TRUE(built.reader->UnorderedValueCounts(0, &c));
  EXPECT_DOUBLE_EQ(SumCounts(c), 100.0);
}

// Workload: an SST whose UNORDERED attr is NULL on every row.
// Threat: with no interned values there is no equality material; exposing an
//         empty-but-true result would make callers treat "no data" as "zero
//         selectivity for every value".
TEST(SabiValueCounts, AllNullAttrHasNoCounts) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 50; ++i) rows.push_back({std::monostate{}});
  BuiltIndex built = BuildIndex(UnorderedOptions(/*nullable=*/true), rows);

  UnorderedAttrValueCounts c;
  EXPECT_FALSE(built.reader->UnorderedValueCounts(0, &c));
}

TEST(SabiHistogram, V7CountsComeFromDirectoryNotBitmaps) {
  std::vector<std::vector<Attr>> rows;
  for (int64_t i = 0; i < 1000; ++i) rows.push_back({i});
  auto built = BuildIndex(I64Options(), rows);
  // v7 blobs persist counts; the histogram must equal the decoded truth.
  ASSERT_FALSE(built.reader->bin_cardinalities.empty());
  OrderedAttrHistogram h;
  ASSERT_TRUE(built.reader->OrderedHistogram(0, &h));
  uint32_t off = 0;  // attr 0 → flat offset 0
  for (size_t b = 0; b < h.counts.size(); ++b)
    EXPECT_EQ(h.counts[b],
              built.reader->bitmap_index.bitmaps[off + b].cardinality());
}
