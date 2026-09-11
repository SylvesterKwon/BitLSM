#include <sabi.h>
#include <sys/types.h>

#include "util/coding.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace {

string IndexTypesToString(const vector<bit_lsm::IndexType>& index_types) {
  string out = "[";
  for (size_t i = 0; i < index_types.size(); ++i) {
    if (i) out += ",";
    out +=
        index_types[i] == bit_lsm::IndexType::kRange ? "kRange" : "kEquality";
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
  // Ungated public entry point (part of the base UserDefinedIndexFactory
  // interface): always kResident. Only the validating overload below -- the
  // path RocksDB actually calls to open an SST -- may mint a kMetadata
  // reader, because the version gate lives there.
  return unique_ptr<SABIReader>(
      new SABIReader(index_block_, SABIReaderMode::kResident));
}

bool SABIFactory::ProducesMetadataOnlyReaders() const {
  return options_.ondemand_index;
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
                              to_string(version) + " (this build reads v" +
                              to_string(kBitLSMFormatVersion) +
                              "); rebuild the DB");
  }

  // Validate the directory prefix (attr_num + index_types) this method
  // interprets; the rest of the directory is parsed by SABIReader against a
  // blob that already passed RocksDB's block checksum.
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
  vector<IndexType> index_types(attr_num);
  for (uint32_t i = 0; i < attr_num; ++i) {
    uint8_t type_byte = static_cast<uint8_t>(dir[sizeof(uint32_t) + i]);
    if (type_byte != static_cast<uint8_t>(IndexType::kEquality) &&
        type_byte != static_cast<uint8_t>(IndexType::kRange)) {
      return Status::Corruption("SABI directory has unknown index type " +
                                to_string(type_byte) + " for attr " +
                                to_string(i));
    }
    index_types[i] = static_cast<IndexType>(type_byte);
  }

  // A schema-bound factory (standalone BitLSM) still rejects mismatches
  // loudly: parsing would succeed with the SST's own index_types, but queries
  // encoded under the configured index_types would silently prune wrong. A
  // schema-less factory (MyRocks reader path) trusts the directory.
  if (!schema_.index_types.empty() && schema_.index_types != index_types) {
    return Status::Corruption("SABI schema mismatch: SST index_types " +
                              IndexTypesToString(index_types) +
                              " vs configured " +
                              IndexTypesToString(schema_.index_types) +
                              "; rebuild the DB or fix the schema");
  }
  // Mode selection happens here, after the version gate above, not in the
  // ungated single-arg NewReader(): a kMetadata reader can only be minted
  // once this method has confirmed the blob's version.
  reader = std::make_unique<SABIReader>(
      index_block, options_.ondemand_index ? SABIReaderMode::kMetadata
                                           : SABIReaderMode::kResident);
  return Status::OK();
}

}  // namespace bit_lsm
