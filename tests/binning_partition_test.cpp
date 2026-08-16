#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/user_defined_index.h>

#include <set>
#include <string>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_utils.h"
#include "sabi.h"
#include "test_util/bitlsm_test_base.h"
#include "test_util/status_matchers.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

// Single ORDERED double attr, the shape passenger_count has in the taxi
// workload: a scalar column whose values are in fact a handful of integers.
BitLSMOptions F64Options() {
  BitLSMOptions o;
  o.attr_num = 1;
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, /*is_signed=*/true,
                           /*is_float=*/true)};
  o.read_seqno = 0;
  o.rho = 0.02;  // bin budget 50, well past the value count
  return o;
}

// Builder plus the reader over its blob; the builder owns the bytes.
struct BuiltIndex {
  std::unique_ptr<SABIBuilder> builder;
  std::unique_ptr<SABIReader> reader;
};

BuiltIndex BuildF64Index(const std::vector<double>& values) {
  BitLSMOptions options = F64Options();
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

// Candidates, not verified rows: this is a precision test, and Verified mode
// would filter the false positives away before they could be counted.
size_t CountCandidates(BitLSM& db, const BitLSMQuery& query) {
  const rocksdb::Snapshot* snap = db.GetInternalDB()->GetSnapshot();
  size_t n = 0;
  {
    BitLSMQuery q = query;
    auto it = db.NewIterator(q, /*cfh=*/nullptr, ResultMode::Candidate, snap);
    EXPECT_NE(it, nullptr);
    if (it != nullptr)
      for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
  }
  db.GetInternalDB()->ReleaseSnapshot(snap);
  return n;
}

}  // namespace

// Workload: an ORDERED attribute holding two values, with three quarters of
//           the rows on the upper one, flushed to an SST so the query goes
//           through the bitmap; then a strict `< 1.0`.
// Threat: thresholds sit between values, so a bin whose lower threshold is the
//         comparand holds only values at or above it and can satisfy no strict
//         less-than. Reading it anyway costs every row it holds -- here the
//         three quarters sitting on 1.0. Results stay correct either way, so
//         only the candidate count shows it.
TEST_F(BitLSMTestBase, StrictLessThanExcludesTheBinItsBoundStartsAt) {
  BitLSMOptions options = DefaultOptions();
  options.rho = 0.1;  // ample bin budget; the attribute's 2 values cap it at 2
  auto& db = OpenDB(options);

  constexpr int kLow = 50;    // attr0 == 0.0
  constexpr int kHigh = 150;  // attr0 == 1.0, the point mass
  for (int i = 0; i < kLow; ++i)
    BITLSM_ASSERT_OK(
        db.Put("lo" + std::to_string(i), {0.0, std::string("x")}, "p"));
  for (int i = 0; i < kHigh; ++i)
    BITLSM_ASSERT_OK(
        db.Put("hi" + std::to_string(i), {1.0, std::string("x")}, "p"));
  BITLSM_ASSERT_OK(db.GetInternalDB()->Flush(rocksdb::FlushOptions()));

  const BitLSMQuery less(
      std::vector<QueryCondition>{{0, CompareOp::LESS, 1.0}});
  const BitLSMQuery less_equal(
      std::vector<QueryCondition>{{0, CompareOp::LESS_EQUAL, 1.0}});

  // `< 1.0` matches only the 0.0 rows, and no bin below the 1.0 threshold can
  // hold anything else, so the candidate set is exactly those rows.
  EXPECT_EQ(CountCandidates(db, less), static_cast<size_t>(kLow));
  // `<= 1.0` must still take the whole table; the two operators are no longer
  // handled by the same branch, so this is the guard on the other side.
  EXPECT_EQ(CountCandidates(db, less_equal), static_cast<size_t>(kLow + kHigh));

  // The upper half of the same story, recorded because it does not come out
  // symmetric. Bins are half-open [lo, hi), so a bin's lower edge is a stored
  // threshold and its upper edge is only the next bin's lower edge. `>= 1.0`
  // therefore starts exactly at the 1.0 bin and is tight, while `> 0.0` has to
  // keep the bin that 0.0 sits in -- proving that bin holds nothing above 0.0
  // would need its contents, not its thresholds. Tightening that is what a
  // per-bin min/max would buy.
  const BitLSMQuery greater_equal(
      std::vector<QueryCondition>{{0, CompareOp::GREATER_EQUAL, 1.0}});
  const BitLSMQuery greater(
      std::vector<QueryCondition>{{0, CompareOp::GREATER, 0.0}});
  EXPECT_EQ(CountCandidates(db, greater_equal), static_cast<size_t>(kHigh));
  EXPECT_EQ(CountCandidates(db, greater), static_cast<size_t>(kLow + kHigh))
      << "strict > got tighter than the thresholds can justify";
}

// Workload: an ORDERED double attribute carrying integers 0..6 in the skewed
//           mix a taxi passenger_count has.
// Threat: the range logic assumes a threshold marks a value, not a position
//         near one -- that assumption is what lets a strict `<` drop the bin
//         its comparand starts. Cutting between values guarantees it; deriving
//         thresholds from interpolated quantiles did not. Assert the property
//         itself rather than one query that happens to expose its absence.
TEST(BinningBoundarySnap, ThresholdsLandOnValuesThatOccur) {
  std::vector<double> values;
  const int mix[] = {6, 70, 12, 5, 3, 2, 2};  // percent per passenger count
  for (int v = 0; v < 7; ++v)
    for (int n = 0; n < mix[v] * 60; ++n)
      values.push_back(static_cast<double>(v));
  BuiltIndex built = BuildF64Index(values);

  std::set<uint64_t> okeys;
  for (double v : values) okeys.insert(F64ToOkey(v));

  const auto& boundaries = std::get<std::vector<uint64_t>>(
      built.reader->bitmap_index.binning_policy[0]);
  ASSERT_FALSE(boundaries.empty());
  for (size_t j = 0; j < boundaries.size(); ++j) {
    EXPECT_TRUE(okeys.count(boundaries[j]) > 0)
        << "threshold " << j << " = " << OkeyToF64(boundaries[j])
        << " is not a value any row holds";
  }
}
