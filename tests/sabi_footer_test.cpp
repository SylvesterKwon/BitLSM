#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "bit_lsm_encoding.h"
#include "bit_lsm_utils.h"
#include "sabi.h"

using namespace bit_lsm;
using UDIB = rocksdb::UserDefinedIndexBuilder;

namespace {

BitLSMOptions TwoAttrOpts() {
  BitLSMOptions o;
  o.attr_num = 2;
  o.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8, /*nullable=*/false),
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary)};
  o.rho = 0.5;
  return o;
}

// Minimal blob: a few rows -> one AddIndexEntry -> Finish
std::string BuildBlob(const BitLSMOptions& o) {
  const ValueLayout layout(o);
  SABIBuilder builder(SABISchema::FromOptions(o),
                      std::make_unique<ValueLayoutExtractor>(o));
  std::string row;
  for (int i = 0; i < 8; ++i) {
    EncodeValue(layout,
                {Attr(double(i)), Attr(std::string("v") + std::to_string(i))},
                "", row);
    builder.OnKeyAdded(rocksdb::Slice("k" + std::to_string(i)),
                       UDIB::ValueType::kValue, rocksdb::Slice(row));
  }
  std::string scratch;
  UDIB::BlockHandle bh{0, 100};
  builder.AddIndexEntry(rocksdb::Slice("k7"), nullptr, bh, &scratch);
  rocksdb::Slice contents;
  EXPECT_TRUE(builder.Finish(&contents).ok());
  return std::string(contents.data(), contents.size());
}

rocksdb::Status OpenViaFactory(
    const SABIFactory& f, std::string blob,
    std::unique_ptr<rocksdb::UserDefinedIndexReader>* reader_out = nullptr) {
  rocksdb::Slice s(blob);
  std::unique_ptr<rocksdb::UserDefinedIndexReader> reader;
  rocksdb::Status status =
      f.NewReader(rocksdb::UserDefinedIndexOption{}, s, reader);
  if (reader_out) *reader_out = std::move(reader);
  return status;
}

}  // namespace

TEST(SabiFooter, AcceptsMatchingSchema) {
  BitLSMOptions o = TwoAttrOpts();
  EXPECT_TRUE(OpenViaFactory(SABIFactory(o), BuildBlob(o)).ok());
}

// A reader-only factory, as an embedding engine uses before it binds the
// schema.
TEST(SabiFooter, SelfDescribesWithoutSchema) {
  BitLSMOptions o = TwoAttrOpts();
  std::unique_ptr<rocksdb::UserDefinedIndexReader> reader;
  rocksdb::Status s = OpenViaFactory(SABIFactory(), BuildBlob(o), &reader);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_NE(reader, nullptr);
  const SABIReader& sabi = *static_cast<SABIReader*>(reader.get());
  ASSERT_EQ(sabi.schema().attr_num(), 2u);
  EXPECT_EQ(sabi.schema().index_types[0], IndexType::kRange);
  EXPECT_EQ(sabi.schema().index_types[1], IndexType::kEquality);
  EXPECT_EQ(sabi.block_handles.size(), 1u);
}

// The index type selects the binning-policy variant at parse time; a
// mismatch would parse string policy bytes as okey thresholds.
TEST(SabiFooter, RejectsRoleFlip) {
  BitLSMOptions build_o = TwoAttrOpts();
  BitLSMOptions read_o = TwoAttrOpts();
  read_o.attr_specs[1] = AttrSpec(IndexType::kRange, PhysicalType::kFloat,
                                  8);  // kEquality->kRange
  rocksdb::Status s = OpenViaFactory(SABIFactory(read_o), BuildBlob(build_o));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(SabiFooter, RejectsWrongVersion) {
  BitLSMOptions o = TwoAttrOpts();
  std::string blob = BuildBlob(o);
  // Patch the version field (second-to-last u32).
  std::string patched = blob;
  uint32_t older_version = kBitLSMFormatVersion - 1;
  std::memcpy(patched.data() + patched.size() - 2 * sizeof(uint32_t),
              &older_version, sizeof(uint32_t));
  rocksdb::Status s = OpenViaFactory(SABIFactory(o), patched);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST(SabiFooter, RejectsUnknownRoleByte) {
  BitLSMOptions o = TwoAttrOpts();
  std::string blob = BuildBlob(o);
  // directory_off is the third-to-last u32; index-type bytes start right after
  // the directory's leading attr_num u32.
  uint32_t directory_off;
  std::memcpy(&directory_off, blob.data() + blob.size() - 3 * sizeof(uint32_t),
              sizeof(uint32_t));
  blob[directory_off + sizeof(uint32_t)] = 7;
  rocksdb::Status s = OpenViaFactory(SABIFactory(o), blob);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// Physical type and nullable are adapter-private; changing them must not
// invalidate existing SSTs.
TEST(SabiFooter, IgnoresAdapterPrivateSpecChanges) {
  BitLSMOptions build_o = TwoAttrOpts();
  BitLSMOptions read_o = TwoAttrOpts();
  read_o.attr_specs[0] = AttrSpec(IndexType::kRange, PhysicalType::kInt,
                                  8);    // same index types
  read_o.attr_specs[0].nullable = true;  // NULL is a dynamic signal, unhashed
  EXPECT_TRUE(OpenViaFactory(SABIFactory(read_o), BuildBlob(build_o)).ok());
}
