#include <sabi.h>
#include <sys/types.h>

#include "util/coding.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace {

string RolesToString(const vector<bit_lsm::AttrRole>& roles) {
  string out = "[";
  for (size_t i = 0; i < roles.size(); ++i) {
    if (i) out += ",";
    out += roles[i] == bit_lsm::AttrRole::ORDERED ? "ORDERED" : "UNORDERED";
  }
  return out + "]";
}

}  // namespace

namespace bit_lsm {

// ========================================================================
// SABIFactory Implementation
// ========================================================================

const char* SABIFactory::Name() const { return "SABIFactory"; }

UserDefinedIndexBuilder* SABIFactory::NewBuilder() const {
  assert(extractor_factory_);  // reader-only factories cannot build
  return new SABIBuilder(schema_, extractor_factory_());
}

unique_ptr<UserDefinedIndexReader> SABIFactory::NewReader(
    Slice& index_block_) const {
  return unique_ptr<SABIReader>(new SABIReader(index_block_));
}

Status SABIFactory::NewReader(
    const UserDefinedIndexOption& /*option*/, Slice& index_block,
    unique_ptr<UserDefinedIndexReader>& reader) const {
  constexpr size_t kFooterSize = 3 * sizeof(uint32_t);
  if (index_block.size() < kFooterSize ||
      DecodeFixed32(index_block.data() + index_block.size() -
                    sizeof(uint32_t)) != kSABIFooterMagic) {
    return Status::Corruption(
        "SABI index has no version footer (written by a pre-versioning "
        "BitLSM build); rebuild the DB");
  }
  uint32_t version = DecodeFixed32(index_block.data() + index_block.size() -
                                   2 * sizeof(uint32_t));
  if (version != kBitLSMFormatVersion) {
    return Status::Corruption("unsupported BitLSM format version " +
                              to_string(version) + " (this build reads " +
                              to_string(kBitLSMFormatVersion) + ")");
  }

  // Validate the directory prefix (attr_num + roles) this method interprets;
  // the rest of the directory is parsed by SABIReader against a blob that
  // already passed RocksDB's block checksum.
  uint32_t directory_off = DecodeFixed32(
      index_block.data() + index_block.size() - 3 * sizeof(uint32_t));
  uint64_t body_end = index_block.size() - kFooterSize;
  if (uint64_t{directory_off} + sizeof(uint32_t) > body_end) {
    return Status::Corruption("SABI directory offset out of bounds");
  }
  const char* dir = index_block.data() + directory_off;
  uint32_t attr_num = DecodeFixed32(dir);
  if (uint64_t{directory_off} + sizeof(uint32_t) + attr_num > body_end) {
    return Status::Corruption("SABI directory is truncated");
  }
  vector<AttrRole> roles(attr_num);
  for (uint32_t i = 0; i < attr_num; ++i) {
    uint8_t role_byte = static_cast<uint8_t>(dir[sizeof(uint32_t) + i]);
    if (role_byte != AttrRole::UNORDERED && role_byte != AttrRole::ORDERED) {
      return Status::Corruption("SABI directory has unknown attr role " +
                                to_string(role_byte) + " for attr " +
                                to_string(i));
    }
    roles[i] = static_cast<AttrRole>(role_byte);
  }

  // A schema-bound factory (standalone BitLSM) still rejects mismatches
  // loudly: parsing would succeed with the SST's own roles, but queries
  // encoded under the configured roles would silently prune wrong. A
  // schema-less factory (MyRocks reader path) trusts the directory.
  if (!schema_.roles.empty() && schema_.roles != roles) {
    return Status::Corruption("SABI schema mismatch: SST roles " +
                              RolesToString(roles) + " vs configured " +
                              RolesToString(schema_.roles) +
                              "; rebuild the DB or fix the schema");
  }
  reader = NewReader(index_block);
  return Status::OK();
}

}  // namespace bit_lsm
