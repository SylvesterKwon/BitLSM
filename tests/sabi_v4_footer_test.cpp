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

rocksdb::Status OpenViaFactory(const SABIFactory& f, std::string blob) {
  rocksdb::Slice s(blob);
  std::unique_ptr<rocksdb::UserDefinedIndexReader> reader;
  return f.NewReader(rocksdb::UserDefinedIndexOption{}, s, reader);
}

}  // namespace

// Workload: build a blob and reopen it through the factory with the same
//           schema.
// Threat: an over-eager footer check that rejects its own writer's output
//         would brick every freshly flushed SST.
TEST(SabiV4Footer, AcceptsMatchingSchema) {
  BitLSMOptions o = TwoAttrOpts();
  EXPECT_TRUE(OpenViaFactory(SABIFactory(o), BuildBlob(o)).ok());
}

// Workload: build with [ORDERED, UNORDERED], reopen with attr 1 flipped to
//           ORDERED.
// Threat: role selects the binning-policy variant at parse time — a silent
//         mismatch parses string policy bytes as okey thresholds.
TEST(SabiV4Footer, RejectsRoleFlip) {
  BitLSMOptions build_o = TwoAttrOpts();
  BitLSMOptions read_o = TwoAttrOpts();
  read_o.attr_specs[1] = AttrSpec(AttrRole::ORDERED);  // UNORDERED->ORDERED
  rocksdb::Status s = OpenViaFactory(SABIFactory(read_o), BuildBlob(build_o));
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// Workload: a valid blob whose version field is patched back to 3.
// Threat: a v3 blob (double boundaries) parsed as v4 (okey) reads garbage
//         thresholds — must be rejected at open, not misparsed.
TEST(SabiV4Footer, RejectsWrongVersion) {
  BitLSMOptions o = TwoAttrOpts();
  std::string blob = BuildBlob(o);
  // Patch the version field (second-to-last u32) back to 3
  std::string patched = blob;
  uint32_t old_version = 3;
  std::memcpy(patched.data() + patched.size() - 2 * sizeof(uint32_t),
              &old_version, sizeof(uint32_t));
  rocksdb::Status s = OpenViaFactory(SABIFactory(o), patched);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// Workload: reopen a blob with width/is_float/nullable changed but roles
//           identical.
// Threat: hashing adapter-private spec fields would invalidate every existing
//         SST on changes that don't affect blob interpretation (row-layout
//         evolution is the adapter's own versioning concern).
TEST(SabiV4Footer, IgnoresAdapterPrivateSpecChanges) {
  BitLSMOptions build_o = TwoAttrOpts();
  BitLSMOptions read_o = TwoAttrOpts();
  read_o.attr_specs[0].is_float = false;  // same schema in the okey domain
  read_o.attr_specs[0].nullable = true;   // NULL is a dynamic signal, unhashed
  EXPECT_TRUE(OpenViaFactory(SABIFactory(read_o), BuildBlob(build_o)).ok());
}
