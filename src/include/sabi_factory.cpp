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
  return new SABIBuilder(options_);
}

unique_ptr<UserDefinedIndexReader> SABIFactory::NewReader(
    Slice& index_block_) const {
  return unique_ptr<SABIReader>(new SABIReader(index_block_, options_));
}

Status SABIFactory::NewReader(
    const UserDefinedIndexOption& /*option*/, Slice& index_block,
    unique_ptr<UserDefinedIndexReader>& reader) const {
  if (index_block.size() < 5 * sizeof(uint32_t) ||
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
  reader = NewReader(index_block);
  return Status::OK();
}

}  // namespace bit_lsm
