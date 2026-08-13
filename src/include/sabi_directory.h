#pragma once
// Per-SSTable SABI directory, held for as long as the DB is open.
//
// Everything in a SABI blob except the bitmaps is small: the schema residue,
// the per-attribute bin counts and binning policies, the index-entry table
// (Section A), and the extent of every bitmap. That metadata is what a query
// needs before it knows which bins to touch, so keeping it resident here --
// and nothing else -- is the same split Cassandra's V1SSTableIndex makes
// between per-SSTable metadata and on-disk components.
//
// Holding it outside the block cache matters: SABIReader parks its parsed
// state inside the cache entry that owns the blob, so evicting the entry
// destroys the metadata too and the next query reloads and reparses the whole
// index. A directory here survives eviction, and a lookup then costs only the
// bins it reads.
//
// Size: 12 bytes per data block plus a word per bin, so it scales with the
// SSTable's block count rather than its index size. This is resident memory
// outside the block-cache budget, in the same category as RocksDB's own
// per-table reader state, and ApproximateMemoryUsage reports it so a run can
// state the figure instead of assuming it is negligible.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "bit_lsm_encoding.h"
#include "rocksdb/cache.h"
#include "rocksdb/listener.h"
#include "sabi.h"
#include "table/format.h"

namespace rocksdb {
class BlockBasedTable;
}

namespace bit_lsm {

class SABIBlobSource;

struct SABIDirectory {
  // Where the blob lives inside the SST file.
  uint64_t blob_offset = 0;
  uint64_t blob_size = 0;

  SABISchema schema;  // attr roles, parsed from the directory
  // Bin counts and binning policies, in the same struct the resident reader
  // fills, so pruning and bin selection run the same code in both modes. Its
  // `bitmaps` vector stays empty here -- that is the whole point.
  BitmapIndex bitmap_index;
  std::vector<uint64_t> distinct_cnts;  // v6+; zeros mean unknown
  // Extent of every bitmap, tombstone last: bitmap i occupies
  // [bitmap_offsets[i], bitmap_offsets[i + 1]).
  std::vector<uint32_t> bitmap_offsets;
  // Section A: rows cumulative through block i, and the block's handle.
  std::vector<uint32_t> data_entries_cnt_psum;
  std::vector<rocksdb::BlockHandle> block_handles;

  uint32_t TotalBins() const {
    return bitmap_offsets.empty()
               ? 0
               : static_cast<uint32_t>(bitmap_offsets.size() - 2);
  }
  uint32_t TombstoneBinIndex() const { return TotalBins(); }
  // First bin index of attr_idx in the flat bin numbering.
  uint32_t AttrBinOffset(uint32_t attr_idx) const {
    uint32_t off = 0;
    const auto& nums = bitmap_index.bitmap_nums;
    for (uint32_t i = 0; i < attr_idx && i < nums.size(); ++i) off += nums[i];
    return off;
  }
  size_t ApproximateMemoryUsage() const;
};

// Parses everything but the bitmaps out of a blob. Exposed so a test can build
// a directory over an in-memory blob, with no SST file.
bool BuildSABIDirectory(SABIBlobSource& src, SABIDirectory* out);

class SABIDirectoryRegistry {
 public:
  // Directory for `file_number`, parsed on first use. Null if the file carries
  // no readable SABI blob.
  const SABIDirectory* Get(uint64_t file_number, rocksdb::BlockBasedTable* bbt,
                           std::shared_ptr<rocksdb::Cache> cache);
  // Called when an SST is deleted; without it a long-running write workload
  // would accumulate directories for files that no longer exist.
  void Drop(uint64_t file_number);
  size_t ApproximateMemoryUsage() const;
  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, std::unique_ptr<SABIDirectory>> map_;
};

// Drops registry entries as compaction and flush retire their SSTables.
class SABIRegistryCleaner : public rocksdb::EventListener {
 public:
  explicit SABIRegistryCleaner(SABIDirectoryRegistry* registry)
      : registry_(registry) {}
  const char* Name() const override { return "SABIRegistryCleaner"; }
  void OnTableFileDeleted(const rocksdb::TableFileDeletionInfo& info) override;

 private:
  SABIDirectoryRegistry* registry_;
};

// How a scan reaches index bytes. A null registry (the default) selects the
// resident SABIReader; a non-null one selects on-demand bitmap reads.
struct SABIIndexContext {
  SABIDirectoryRegistry* registry = nullptr;
  std::shared_ptr<rocksdb::Cache> cache;
};

}  // namespace bit_lsm
