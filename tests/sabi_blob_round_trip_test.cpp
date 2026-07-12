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
}  // namespace

// SABIBuilder 에 값들을 먹이고 Finish 로 블롭을 만든 뒤 SABIReader 로 다시
// 파싱했을 때 블록 핸들/카운트/비닝 정책/비트맵 개수가 일관적인지.
TEST(SabiBlobRoundTrip, BuildsAndParses) {
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
  BITLSM_ASSERT_OK(builder.Finish(&blob));

  // 블롭 메모리는 builder 가 소유하므로 builder 가 스코프에 살아있는 동안 파싱.
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
