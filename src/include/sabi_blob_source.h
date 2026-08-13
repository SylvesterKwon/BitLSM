#pragma once
// Byte-range access to one SSTable's SABI blob.
//
// RocksDB hands a user-defined index to its reader as a single block, so
// SABIReader materialises the whole blob: every bin's frozen bitmap is copied
// into an aligned buffer at open, and the parsed reader lives inside the cache
// entry. That is free when the index fits in memory and ruinous when it does
// not -- an evicted entry costs a full re-read and re-parse of the file's
// entire index on the next query, however few bins that query touches.
//
// A source is the alternative: read the ranges a lookup actually needs. Only
// the bitmaps are large, and the blob's directory records each one's extent,
// so a query reads its bins and nothing else. Pages are cached in the same
// block cache the data blocks compete for, so index residency answers to the
// same budget.
//
// Reading a range out of the middle of the blob is possible because
// BlockBasedTableBuilder writes BlockType::kUserDefinedIndex uncompressed.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "rocksdb/cache.h"
#include "rocksdb/slice.h"

namespace rocksdb {
class RandomAccessFileReader;
}

namespace bit_lsm {

// Matches the 4 KB default data-block size, so index pages and data blocks
// occupy the block cache at the same granularity.
inline constexpr uint32_t kSABIBlobPageSize = 4096;

// Process-wide totals for the on-demand path. Counted here rather than through
// RocksDB's tickers because these reads bypass BlockBasedTable.
struct SABIBlobSourceStats {
  uint64_t page_hits = 0;    // page served from the block cache
  uint64_t page_misses = 0;  // page read from the SST file
  uint64_t bytes_read = 0;   // bytes pulled off disk (whole pages)
  uint64_t bitmaps_loaded = 0;
};
SABIBlobSourceStats GetSABIBlobSourceStats();
void ResetSABIBlobSourceStats();

class SABIBlobSource {
 public:
  // `file` is owned by the table reader and `cache` by the table options; both
  // outlive a source, which lives for one scan of one file.
  SABIBlobSource(rocksdb::RandomAccessFileReader* file, uint64_t blob_offset,
                 uint64_t blob_size, uint64_t file_number,
                 std::shared_ptr<rocksdb::Cache> cache)
      : file_(file),
        blob_offset_(blob_offset),
        blob_size_(blob_size),
        file_number_(file_number),
        cache_(std::move(cache)) {}

  // Copies [rel_off, rel_off + len) into `scratch` and returns a pointer to
  // it. Copying is deliberate: a cached page can be evicted as soon as its
  // handle is released, so handing out pointers into cache memory would make
  // every caller a lifetime problem.
  const char* Read(uint32_t rel_off, uint32_t len, std::string& scratch);

  // Copies a bitmap's bytes into a fresh 32-byte-aligned buffer, which is what
  // roaring::Roaring::frozenView requires. The caller owns the buffer and must
  // keep it alive for as long as the view it builds over it.
  using AlignedPtr = std::unique_ptr<char[], void (*)(void*)>;
  AlignedPtr ReadAligned(uint32_t rel_off, uint32_t len);

  uint64_t BlobSize() const { return blob_size_; }
  bool ok() const { return ok_; }

 private:
  // Copies one page's [in_page, in_page + n) into `dst`.
  bool ReadFromPage(uint64_t page_idx, uint32_t page_len, uint32_t in_page,
                    uint32_t n, char* dst);

  rocksdb::RandomAccessFileReader* file_;
  uint64_t blob_offset_;
  uint64_t blob_size_;
  uint64_t file_number_;
  std::shared_ptr<rocksdb::Cache> cache_;
  bool ok_ = true;
};

}  // namespace bit_lsm
