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
  o.attr_specs = {AttrSpec(AttrRole::ORDERED, 8, true, true, false),
                  AttrSpec(AttrRole::UNORDERED)};
  o.rho = 0.5;
  return o;
}

// Minimal blob: a few rows -> one AddIndexEntry -> Finish
std::string BuildBlob(const BitLSMOptions& o) {
  SABIBuilder builder(SABISchema::FromOptions(o),
                      std::make_unique<ValueLayoutExtractor>(o));
  std::string row;
  for (int i = 0; i < 8; ++i) {
    EncodeValue(o,
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

// Workload: build a blob and reopen it through the factory with the same
//           schema.
// Threat: an over-eager footer check that rejects its own writer's output
//         would brick every freshly flushed SST.
TEST(SabiV5Footer, AcceptsMatchingSchema) {
  BitLSMOptions o = TwoAttrOpts();
  EXPECT_TRUE(OpenViaFactory(SABIFactory(o), BuildBlob(o)).ok());
}

// Workload: reopen a blob through a schema-less (reader-only) factory, as a
//           restarting MyRocks does before DDL binding.
// Threat: a reader that still needs an injected schema fails every restart
//         with flushed SABI SSTs — the v4 chicken-and-egg DB-open bug.
TEST(SabiV5Footer, SelfDescribesWithoutSchema) {
  BitLSMOptions o = TwoAttrOpts();
  std::unique_ptr<rocksdb::UserDefinedIndexReader> reader;
  rocksdb::Status s = OpenViaFactory(SABIFactory(), BuildBlob(o), &reader);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_NE(reader, nullptr);
  const SABIReader& sabi = *static_cast<SABIReader*>(reader.get());
  ASSERT_EQ(sabi.schema().attr_num(), 2u);
  EXPECT_EQ(sabi.schema().roles[0], AttrRole::ORDERED);
  EXPECT_EQ(sabi.schema().roles[1], AttrRole::UNORDERED);
  EXPECT_EQ(sabi.block_handles.size(), 1u);
}

// Workload: build with [ORDERED, UNORDERED], reopen with attr 1 flipped to
//           ORDERED.
// Threat: role selects the binning-policy variant at parse time — a silent
//         mismatch parses string policy bytes as okey thresholds.
TEST(SabiV5Footer, RejectsRoleFlip) {
  BitLSMOptions build_o = TwoAttrOpts();
  BitLSMOptions read_o = TwoAttrOpts();
  read_o.attr_specs[1] = AttrSpec(AttrRole::ORDERED);  // UNORDERED->ORDERED
  rocksdb::Status s = OpenViaFactory(SABIFactory(read_o), BuildBlob(build_o));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// Workload: a valid blob whose version field is patched back to 4.
// Threat: a v4 blob (24B footer, schema hash, no directory) parsed as v5
//         reads a garbage directory offset — must be rejected at open.
TEST(SabiV5Footer, RejectsWrongVersion) {
  BitLSMOptions o = TwoAttrOpts();
  std::string blob = BuildBlob(o);
  // Patch the version field (second-to-last u32) back to 4
  std::string patched = blob;
  uint32_t old_version = 4;
  std::memcpy(patched.data() + patched.size() - 2 * sizeof(uint32_t),
              &old_version, sizeof(uint32_t));
  rocksdb::Status s = OpenViaFactory(SABIFactory(o), patched);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// Workload: a valid blob whose first stored role byte is patched to a value
//           outside the AttrRole enum.
// Threat: the directory is now the source of truth for roles — an
//         unrecognized role byte must fail loudly at open, not fall through
//         parse branches as an arbitrary role.
TEST(SabiV5Footer, RejectsUnknownRoleByte) {
  BitLSMOptions o = TwoAttrOpts();
  std::string blob = BuildBlob(o);
  // directory_off is the third-to-last u32; role bytes start right after the
  // directory's leading attr_num u32.
  uint32_t directory_off;
  std::memcpy(&directory_off, blob.data() + blob.size() - 3 * sizeof(uint32_t),
              sizeof(uint32_t));
  blob[directory_off + sizeof(uint32_t)] = 7;
  rocksdb::Status s = OpenViaFactory(SABIFactory(o), blob);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// Workload: reopen a blob with width/is_float/nullable changed but roles
//           identical.
// Threat: validating adapter-private spec fields would invalidate every
//         existing SST on changes that don't affect blob interpretation
//         (row-layout evolution is the adapter's own versioning concern).
TEST(SabiV5Footer, IgnoresAdapterPrivateSpecChanges) {
  BitLSMOptions build_o = TwoAttrOpts();
  BitLSMOptions read_o = TwoAttrOpts();
  read_o.attr_specs[0].is_float = false;  // same schema in the okey domain
  read_o.attr_specs[0].nullable = true;   // NULL is a dynamic signal, unhashed
  EXPECT_TRUE(OpenViaFactory(SABIFactory(read_o), BuildBlob(build_o)).ok());
}
