#include <folly/Range.h>
#include <sabi.h>
#include <sys/types.h>

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

}  // namespace bit_lsm
