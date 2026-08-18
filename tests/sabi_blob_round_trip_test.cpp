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
  options.attr_specs = {AttrSpec{AttrRole::ORDERED},
                        AttrSpec{AttrRole::UNORDERED}};
  options.read_seqno = 0;
  options.rho = 0.5;  // 전체 bin 예산 = attr_num/rho = 4 (속성별 할당은 동적)
  return options;
}

// Shared fixture: 4 rows over {ORDERED, UNORDERED} and one AddIndexEntry.
// Returns an owned copy of the blob since the builder (owner of the Slice
// memory Finish() points into) goes out of scope on return. When
// out_bitmap_sizes is non-null, filled with the builder's own
// TEST_LastFinishBitmapSizes() (tombstone last) -- ground truth captured
// before serialization, independent of anything a reader later derives from
// the blob's persisted bytes.
std::string BuildTestBlob(std::vector<uint32_t>* out_bitmap_sizes = nullptr) {
  BitLSMOptions options = MakeOptions();
  SABIBuilder builder(SABISchema::FromOptions(options),
                      std::make_unique<ValueLayoutExtractor>(options));

  const std::vector<std::string> keys = {"k0", "k1", "k2", "k3"};
  const std::vector<std::pair<double, std::string>> rows = {
      {1.0, "apple"}, {2.0, "banana"}, {3.0, "apple"}, {4.0, "cherry"}};

  // OnKeyAdded 는 value Slice 로 인코딩 값을 받는다 → 살아있어야 하므로 보관.
  std::vector<std::string> encoded;
  encoded.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string out;
    EncodeValue(options, {rows[i].first, rows[i].second}, "p", out);
    encoded.push_back(std::move(out));
    builder.OnKeyAdded(rocksdb::Slice(keys[i]), UDIB::ValueType::kValue,
                       rocksdb::Slice(encoded[i]));
  }

  // 모든 키를 담는 단일 데이터 블록 하나를 등록.
  // offset/size 는 빌더가 PutFixed32 로 직렬화하므로 32비트에 들어가는 작은
  // 값을 쓴다.
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

// SABIBuilder 에 값들을 먹이고 Finish 로 블롭을 만든 뒤 SABIReader 로 다시
// 파싱했을 때 블록 핸들/카운트/비닝 정책/비트맵 개수가 일관적인지.
TEST(SabiBlobRoundTrip, BuildsAndParses) {
  BitLSMOptions options = MakeOptions();
  std::string blob_owner = BuildTestBlob();
  rocksdb::Slice blob(blob_owner);

  // v5: 리더는 스키마 주입 없이 블롭의 directory 에서 self-describe 한다.
  SABIReader reader(blob);

  // roles 가 블롭에서 그대로 복원되는지.
  ASSERT_EQ(reader.schema().attr_num(), options.attr_num);
  EXPECT_EQ(reader.schema().roles[0], AttrRole::ORDERED);
  EXPECT_EQ(reader.schema().roles[1], AttrRole::UNORDERED);

  // 인덱스 엔트리(블록) 1개.
  ASSERT_EQ(reader.block_handles.size(), 1u);
  EXPECT_EQ(reader.block_handles[0].offset(), 100u);
  EXPECT_EQ(reader.block_handles[0].size(), 4096u);
  ASSERT_EQ(reader.data_entries_cnt_psum.size(), 1u);
  EXPECT_EQ(reader.data_entries_cnt_psum[0],
            4u);  // AddIndexEntry 시점까지 4개 키

  // 비닝 정책과 속성별 bin 수: 둘 다 attr_num 개, bin 수는 각각 최소 1.
  ASSERT_EQ(reader.bitmap_index.binning_policy.size(), options.attr_num);
  ASSERT_EQ(reader.bitmap_index.bitmap_nums.size(), options.attr_num);
  uint32_t total_bins = 0;
  for (uint32_t n : reader.bitmap_index.bitmap_nums) {
    EXPECT_GE(n, 1u);
    total_bins += n;
  }
  // 실제 비트맵 개수(tombstone 제외) == bin 수 총합.
  EXPECT_EQ(reader.bitmap_index.bitmaps.size(), total_bins);
}

// Workload: a freshly built blob validated through the factory's
//           Status-returning NewReader; the same blob with its version footer
//           stripped (pre-versioning layout); and one with a bumped version.
// Threat: a pre-versioned or future-format index gets parsed as if current —
//         garbage offsets / UB instead of a clean Corruption error.
TEST(SabiBlobRoundTrip, RejectsUnversionedAndUnknownVersions) {
  BitLSMOptions options = MakeOptions();
  SABIBuilder builder(SABISchema::FromOptions(options),
                      std::make_unique<ValueLayoutExtractor>(options));

  std::string encoded;
  EncodeValue(options, {1.0, std::string("apple")}, "p", encoded);
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

  // 2. Version footer stripped -> pre-versioning layout -> Corruption.
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

// Workload: the same 4-row/2-attr fixture, inspected for v7's on-disk
//           contract: every bitmap start is 32B-aligned, exact frozen sizes
//           are persisted alongside padded offsets, and per-bin/tombstone
//           cardinalities round-trip against the decoded bitmaps.
// Threat: padding without persisted exact sizes breaks frozenView (it demands
//         the exact frozen length); missing/misaligned cardinalities would
//         force a metadata-only reader to decode bitmaps just to count rows.
TEST(SabiBlobRoundTrip, V7PadsBitmapsAndPersistsSizesAndCardinalities) {
  std::vector<uint32_t> ground_truth_sizes;
  // same fixture body as BuildsAndParses
  std::string blob = BuildTestBlob(&ground_truth_sizes);
  rocksdb::Slice slice(blob);
  bit_lsm::SABIReader reader(slice);

  uint32_t total_bins = 0;
  for (uint32_t n : reader.bitmap_index.bitmap_nums) total_bins += n;
  ASSERT_EQ(reader.TotalBins(), total_bins);

  // v7: every bitmap start (tombstone included) is 32B-aligned in the blob.
  ASSERT_EQ(reader.bitmap_offsets_.size(), total_bins + 2u);
  for (uint32_t i = 0; i <= total_bins; ++i)
    EXPECT_EQ(reader.bitmap_offsets_[i] % 32u, 0u) << "bin " << i;

  // Exact sizes recover the frozen views (frozenView demands exact length),
  // and persisted cardinalities match the decoded bitmaps.
  ASSERT_EQ(reader.bitmap_sizes_.size(), total_bins + 1u);
  ASSERT_EQ(ground_truth_sizes.size(), total_bins + 1u);
  for (uint32_t i = 0; i <= total_bins; ++i)
    EXPECT_EQ(reader.bitmap_sizes_[i], ground_truth_sizes[i])
        << "bin " << i
        << " (persisted size vs. the builder's own pre-serialization "
           "getFrozenSizeInBytes())";
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

// Workload: a minimal v6 blob, hand-serialized field-by-field (NOT produced
//           by the current v7 builder) — one ORDERED attr, 2 bins, unpadded
//           butt-joined bitmap offsets, no per-bin cardinality/size arrays —
//           parsed by the current (v7-aware) SABIReader.
// Threat: every other test in this file builds its blob with today's (v7)
//         builder, so none of them actually exercises a genuine pre-v7
//         directory shape. v7's reader changes (the two new directory
//         arrays, the bitmap_offsets_/bitmap_sizes_ split) must not disturb
//         parsing of the already-flushed v5/v6 SSTs that lack them.
TEST(SabiBlobRoundTrip, ParsesGenuineV6Blob) {
  using roaring::Roaring;

  // 1 ORDERED attr, 2 bins. Rows 0,1 -> bin0; row 2 -> bin1; row 3 ->
  // tombstone. Cardinalities are chosen by hand, not by the builder's binning
  // math.
  Roaring bin0, bin1, tombstone;
  bin0.add(0);
  bin0.add(1);
  bin1.add(2);
  tombstone.add(3);
  std::vector<Roaring*> bitmaps = {&bin0, &bin1, &tombstone};
  for (Roaring* r : bitmaps) r->runOptimize();

  std::string blob;

  // Section A (index entries): one AddIndexEntry-equivalent record:
  // [row-count psum u32][bh.offset u32][bh.size u32].
  rocksdb::PutFixed32(&blob, 4);    // psum: 4 rows seen so far
  rocksdb::PutFixed32(&blob, 7);    // bh.offset
  rocksdb::PutFixed32(&blob, 999);  // bh.size

  // Frozen bitmaps, tombstone last, butt-joined (v6 has no padding between
  // them — offsets are exactly consecutive getFrozenSizeInBytes() sums).
  std::vector<uint32_t> bitmap_sizes(bitmaps.size());
  for (size_t i = 0; i < bitmaps.size(); ++i)
    bitmap_sizes[i] = static_cast<uint32_t>(bitmaps[i]->getFrozenSizeInBytes());
  std::vector<uint32_t> bitmap_offsets(bitmaps.size() + 1);
  bitmap_offsets[0] = static_cast<uint32_t>(blob.size());
  for (size_t i = 0; i < bitmaps.size(); ++i)
    bitmap_offsets[i + 1] = bitmap_offsets[i] + bitmap_sizes[i];
  blob.resize(bitmap_offsets.back());
  for (size_t i = 0; i < bitmaps.size(); ++i)
    bitmaps[i]->writeFrozen(blob.data() + bitmap_offsets[i]);

  // Binning policy: the ORDERED attr's body is headerless -- bin_count+1
  // okey thresholds, chosen by hand (2 bins -> 3 boundaries).
  uint32_t policy_off_start = static_cast<uint32_t>(blob.size());
  for (uint64_t boundary : {uint64_t{0}, uint64_t{10}, uint64_t{20}})
    rocksdb::PutFixed64(&blob, boundary);
  uint32_t policy_off_end = static_cast<uint32_t>(blob.size());

  // Directory: [attr_num][role][bin_count][index_entries_cnt]
  //            [distinct_cnt u64 x attr_num]  (v6+)
  //            [policy offsets][bitmap offsets]
  // -- no bin_cardinality/bitmap_size arrays; those are v7+ only.
  uint32_t directory_off = static_cast<uint32_t>(blob.size());
  rocksdb::PutFixed32(&blob, 1);                         // attr_num
  blob.push_back(static_cast<char>(AttrRole::ORDERED));  // role
  rocksdb::PutFixed32(&blob, 2);                         // bin_count
  rocksdb::PutFixed32(&blob, 1);                         // index_entries_cnt
  rocksdb::PutFixed64(&blob, 3);                         // v6 distinct_cnt
  rocksdb::PutFixed32(&blob, policy_off_start);
  rocksdb::PutFixed32(&blob, policy_off_end);
  for (uint32_t off : bitmap_offsets) rocksdb::PutFixed32(&blob, off);

  // Footer: [directory_off][version=6][magic].
  rocksdb::PutFixed32(&blob, directory_off);
  rocksdb::PutFixed32(&blob, 6);
  rocksdb::PutFixed32(&blob, kSABIFooterMagic);

  rocksdb::Slice slice(blob);
  SABIReader reader(slice);

  // Roles / bin counts round-trip.
  ASSERT_EQ(reader.schema().attr_num(), 1u);
  EXPECT_EQ(reader.schema().roles[0], AttrRole::ORDERED);
  ASSERT_EQ(reader.bitmap_index.bitmap_nums.size(), 1u);
  EXPECT_EQ(reader.bitmap_index.bitmap_nums[0], 2u);
  EXPECT_EQ(reader.TotalBins(), 2u);

  // v6: no persisted per-bin cardinalities.
  EXPECT_TRUE(reader.bin_cardinalities.empty());

  // v6: sizes derived from offset differences (no padding to correct for).
  ASSERT_EQ(reader.bitmap_sizes_.size(), 3u);
  for (uint32_t i = 0; i < 3; ++i)
    EXPECT_EQ(reader.bitmap_sizes_[i],
              reader.bitmap_offsets_[i + 1] - reader.bitmap_offsets_[i]);

  // Decoded bitmaps match what was hand-written, and BinCardinality /
  // TombstoneCardinality fall back to the decoded cardinality.
  ASSERT_EQ(reader.bitmap_index.bitmaps.size(), 2u);
  EXPECT_EQ(reader.bitmap_index.bitmaps[0].cardinality(), 2u);
  EXPECT_EQ(reader.bitmap_index.bitmaps[1].cardinality(), 1u);
  EXPECT_EQ(reader.BinCardinality(0), 2u);
  EXPECT_EQ(reader.BinCardinality(1), 1u);
  EXPECT_EQ(reader.bitmap_index.tombstone_bitmap.cardinality(), 1u);
  EXPECT_EQ(reader.TombstoneCardinality(), 1u);
}
