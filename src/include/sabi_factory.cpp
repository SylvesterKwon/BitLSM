#include <sabi.h>
#include <sys/types.h>

#include "util/coding.h"

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace bit_lsm {

// ========================================================================
// SABIFactory Implementation
// ========================================================================

const char* SABIFactory::Name() const { return "SABIFactory"; }

UserDefinedIndexBuilder* SABIFactory::NewBuilder() const {
  return new SABIBuilder(schema_, extractor_factory_());
}

unique_ptr<UserDefinedIndexReader> SABIFactory::NewReader(
    Slice& index_block_) const {
  return unique_ptr<SABIReader>(new SABIReader(index_block_, schema_));
}

Status SABIFactory::NewReader(
    const UserDefinedIndexOption& /*option*/, Slice& index_block,
    unique_ptr<UserDefinedIndexReader>& reader) const {
  if (index_block.size() < 6 * sizeof(uint32_t) ||
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
  uint32_t stored_hash = DecodeFixed32(index_block.data() + index_block.size() -
                                       3 * sizeof(uint32_t));
  if (stored_hash != schema_.Hash()) {
    return Status::Corruption(
        "SABI schema mismatch: SST was built with a different attr role "
        "configuration than this reader; rebuild the DB or fix the schema");
  }
  reader = NewReader(index_block);
  return Status::OK();
}

}  // namespace bit_lsm
