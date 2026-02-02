#include <folly/Range.h>
#include <folly/stats/TDigest.h>
#include <sabi.h>
#include <sys/types.h>

using namespace std;
using namespace rocksdb;
using namespace roaring;

namespace bitmap_index {

// ========================================================================
// SABIFactory Implementation
// ========================================================================

const char* SABIFactory::Name() const { return "SABIFactory"; }

UserDefinedIndexBuilder* SABIFactory::NewBuilder() const {
  return new SABIBuilder(options_);
}

unique_ptr<UserDefinedIndexReader>
SABIFactory::NewReader(Slice& index_block_) const {
  return unique_ptr<SABIReader>(new SABIReader(index_block_, options_));
}

} // namespace bitmap_index
