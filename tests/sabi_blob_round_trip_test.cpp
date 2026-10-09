#include <gtest/gtest.h>
#include <rocksdb/slice.h>
#include <rocksdb/user_defined_index.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "bit_lsm_encoding.h"
#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "sabi.h"
#include "test_util/status_matchers.h"
#include "util/coding.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {
BitLSMOptions MakeOptions() {
  BitLSMOptions options;
  options.attr_num = 2;
  options.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  options.read_seqno = 0;
  options.rho = 0.5;  // bin budget = attr_num/rho = 4, split per attr
  return options;
}

// Shared fixture: 4 rows over {kRange, kEquality} and one AddIndexEntry.
// Returns an owned copy of the blob, since the builder owns the memory
// Finish() points into. out_bitmap_sizes, when non-null, receives the
// builder's TEST_LastFinishBitmapSizes().
std::string BuildTestBlob(std::vector<uint32_t>* out_bitmap_sizes = nullptr) {
  BitLSMOptions options = MakeOptions();
  const ValueLayout layout(options);
  SABIBuilder builder(SABISchema::FromOptions(options),
                      std::make_unique<ValueLayoutExtractor>(options));

  const std::vector<std::string> keys = {"k0", "k1", "k2", "k3"};
  const std::vector<std::pair<double, std::string>> rows = {
      {1.0, "apple"}, {2.0, "banana"}, {3.0, "apple"}, {4.0, "cherry"}};

  // OnKeyAdded takes the encoded value by Slice, so keep the buffers alive.
  std::vector<std::string> encoded;
  encoded.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string out;
    EncodeValue(layout, {rows[i].first, rows[i].second}, "p", out);
    encoded.push_back(std::move(out));
    builder.OnKeyAdded(rocksdb::Slice(keys[i]), UDIB::ValueType::kValue,
                       rocksdb::Slice(encoded[i]));
  }

  // One data block holding every key. offset/size are serialized with
  // PutFixed32, so keep them within 32 bits.
  std::string scratch;
  UDIB::BlockHandle block_handle{/*offset=*/100, /*size=*/4096};
  builder.AddIndexEntry(rocksdb::Slice(keys.back()),
                        /*first_key_in_next_block=*/nullptr, block_handle,
                        &scratch);

  rocksdb::Slice blob;
  EXPECT_TRUE(builder.Finish(&blob).ok());
  if (out_bitmap_sizes != nullptr)
    *out_bitmap_sizes = builder.TEST_LastFinishBitmapSizes();
  return std::string(blob.data(), blob.size());
}
}  // namespace

// A blob built by SABIBuilder::Finish parses back through SABIReader with
// consistent block handles, counts, binning policies and bitmap counts.
TEST(SabiBlobRoundTrip, BuildsAndParses) {
  BitLSMOptions options = MakeOptions();
  std::string blob_owner = BuildTestBlob();
  rocksdb::Slice blob(blob_owner);

  // The reader self-describes from the blob's directory; no schema injected.
  SABIReader reader(blob);

  // index_types come back from the blob.
  ASSERT_EQ(reader.schema().attr_num(), options.attr_num);
  EXPECT_EQ(reader.schema().index_types[0], IndexType::kRange);
  EXPECT_EQ(reader.schema().index_types[1], IndexType::kEquality);

  // One index entry (block).
  ASSERT_EQ(reader.block_handles.size(), 1u);
  EXPECT_EQ(reader.block_handles[0].offset(), 100u);
  EXPECT_EQ(reader.block_handles[0].size(), 4096u);
  ASSERT_EQ(reader.data_entries_cnt_psum.size(), 1u);
  EXPECT_EQ(reader.data_entries_cnt_psum[0],
            4u);  // four keys before AddIndexEntry

  // One binning policy and bin count per attr, each attr has at least one bin.
  ASSERT_EQ(reader.bitmap_index.binning_policy.size(), options.attr_num);
  ASSERT_EQ(reader.bitmap_index.bitmap_nums.size(), options.attr_num);
  uint32_t total_bins = 0;
  for (uint32_t n : reader.bitmap_index.bitmap_nums) {
    EXPECT_GE(n, 1u);
    total_bins += n;
  }
  // Bitmap count (tombstone excluded) == total bins.
  EXPECT_EQ(reader.bitmap_index.bitmaps.size(), total_bins);
}

// A blob with its version footer stripped, and one with a bumped
// version, must both fail with Corruption.
TEST(SabiBlobRoundTrip, RejectsUnversionedAndUnknownVersions) {
  BitLSMOptions options = MakeOptions();
  const ValueLayout layout(options);
  SABIBuilder builder(SABISchema::FromOptions(options),
                      std::make_unique<ValueLayoutExtractor>(options));

  std::string encoded;
  EncodeValue(layout, {1.0, std::string("apple")}, "p", encoded);
  builder.OnKeyAdded(rocksdb::Slice("k0"), UDIB::ValueType::kValue,
                     rocksdb::Slice(encoded));
  std::string scratch;
  UDIB::BlockHandle block_handle{/*offset=*/100, /*size=*/4096};
  builder.AddIndexEntry(rocksdb::Slice("k0"), nullptr, block_handle, &scratch);
  rocksdb::Slice blob;
  BITLSM_ASSERT_OK(builder.Finish(&blob));

  SABIFactory factory(options);
  rocksdb::UserDefinedIndexOption udi_option;

  // 1. Current version passes validation.
  std::unique_ptr<rocksdb::UserDefinedIndexReader> reader;
  BITLSM_ASSERT_OK(factory.NewReader(udi_option, blob, reader));
  ASSERT_NE(reader, nullptr);

  // 2. Version footer stripped -> Corruption.
  std::string unversioned(blob.data(), blob.size() - 2 * sizeof(uint32_t));
  rocksdb::Slice unversioned_slice(unversioned);
  reader.reset();
  EXPECT_TRUE(
      factory.NewReader(udi_option, unversioned_slice, reader).IsCorruption());

  // 3. Unknown (future) version number -> Corruption.
  std::string bumped(blob.data(), blob.size());
  uint32_t future_version = kBitLSMFormatVersion + 1;
  std::memcpy(bumped.data() + bumped.size() - 2 * sizeof(uint32_t),
              &future_version, sizeof(uint32_t));
  rocksdb::Slice bumped_slice(bumped);
  reader.reset();
  EXPECT_TRUE(
      factory.NewReader(udi_option, bumped_slice, reader).IsCorruption());
}

// Every bitmap start is 32B-aligned, exact frozen sizes are persisted
// alongside padded offsets, and per-bin cardinalities match the decoded
// bitmaps.
TEST(SabiBlobRoundTrip, PadsBitmapsAndPersistsSizesAndCardinalities) {
  std::vector<uint32_t> ground_truth_sizes;
  // same fixture body as BuildsAndParses
  std::string blob = BuildTestBlob(&ground_truth_sizes);
  rocksdb::Slice slice(blob);
  bit_lsm::SABIReader reader(slice);

  uint32_t total_bins = 0;
  for (uint32_t n : reader.bitmap_index.bitmap_nums) total_bins += n;
  ASSERT_EQ(reader.TotalBins(), total_bins);

  // Every bitmap start (tombstone included) is 32B-aligned in the blob.
  ASSERT_EQ(reader.bitmap_offsets_.size(), total_bins + 2u);
  for (uint32_t i = 0; i <= total_bins; ++i)
    EXPECT_EQ(reader.bitmap_offsets_[i] % 32u, 0u) << "bin " << i;

  // Exact sizes recover the frozen views (frozenView demands exact length),
  // and persisted cardinalities match the decoded bitmaps.
  ASSERT_EQ(reader.bitmap_sizes_.size(), total_bins + 1u);
  ASSERT_EQ(ground_truth_sizes.size(), total_bins + 1u);
  for (uint32_t i = 0; i <= total_bins; ++i)
    EXPECT_EQ(reader.bitmap_sizes_[i], ground_truth_sizes[i]) << "bin " << i;
  ASSERT_EQ(reader.bin_cardinalities.size(), total_bins + 1u);
  for (uint32_t i = 0; i < total_bins; ++i)
    EXPECT_EQ(reader.bin_cardinalities[i],
              reader.bitmap_index.bitmaps[i].cardinality());
  EXPECT_EQ(reader.TombstoneCardinality(),
            reader.bitmap_index.tombstone_bitmap.cardinality());
  for (uint32_t i = 0; i < total_bins; ++i)
    EXPECT_EQ(reader.BinCardinality(i),
              reader.bitmap_index.bitmaps[i].cardinality());
}
