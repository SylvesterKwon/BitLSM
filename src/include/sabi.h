#pragma once

#define TEST_CACHE_LINE_SIZE 64  // To avoid compile error

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "bit_lsm_encoding.h"
#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include "roaring.hh"
#include "rocksdb/advanced_cache.h"  // full rocksdb::Cache / Cache::Handle
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include "table/format.h"

namespace bit_lsm {

// On-disk format version of a BitLSM SST.
// v4: ORDERED binning boundaries are okey (order-preserving uint64), not
//     double; footer carries a SABISchema hash.
// v5: self-describing — the directory persists the schema residue (attr
//     roles), so a reader needs no schema binding (see SABIFactory::NewReader).
//     Corruption detection is RocksDB's block checksum, verified before the
//     blob reaches the factory; the blob carries no checksum of its own.
//
// SABI index blob wire layout (v7), as written by SABIBuilder::Finish:
//   [index entries]     per block: [row-count psum u32][bh.offset u32]
//                       [bh.size u32]
//   [frozen bitmaps]    per-attr value bins in attr order, tombstone bitmap
//                       last. v7+: every bitmap starts on a 32-byte boundary
//                       (blob-relative, first included); gap bytes are zero.
//                       A 32B-aligned copy of the whole region can therefore
//                       back every frozenView in place.
//   [binning policies]  per attr:
//                       ORDERED:   okey threshold u64 x (bin_count+1)
//                       UNORDERED: [entry_count u32] then
//                                  {varint-len string, bin u32} x entry_count
//   [directory]         [attr_num u32][role u8 x attr_num]
//                       [bin_count u32 x attr_num][index_entries_cnt u32]
//                       [distinct_cnt u64 x attr_num]              (v6+)
//                       [bin_cardinality u32 x (total_bins+1)]     (v7+)
//                       [bitmap_size u32 x (total_bins+1)]         (v7+)
//                       [policy offset u32 x (attr_num+1)]
//                       [bitmap offset u32 x (total_bins+2)]
//   [footer]            [directory_off u32][version u32][magic u32]
//
// v6 adds per-attr exact distinct-value counts (NULL/tombstone rows excluded)
// to the directory: the estimator's equality floor (a point estimate is at
// least physical/NDV -- sparse integer domains like yyyymm otherwise smear
// point mass into value holes). Readers accept v5 blobs; their distinct
// counts read as 0 = unknown, which disables the floor for that SST.
//
// v7 adds the padded bitmap layout plus two per-bin arrays: exact frozen
// sizes (padded offsets no longer encode sizes by difference) and
// cardinalities (tombstone last), so the estimator and any metadata-only
// reader never decode a bitmap to count rows. Readers accept v5/v6; their
// bin_cardinalities read as empty = derive from the decoded bitmaps.
inline constexpr uint32_t kBitLSMFormatVersion = 7;
// Oldest on-disk version this build still reads (v5 = pre-NDV directory).
inline constexpr uint32_t kBitLSMMinReadFormatVersion = 5;
// Trailing magic of the SABI blob footer
inline constexpr uint32_t kSABIFooterMagic =
    0x5AB1B175;  // SABIBITS in hexspeak :^)

struct BitmapIndex {
  // Bitmap
  std::vector<roaring::Roaring> bitmaps;
  roaring::Roaring tombstone_bitmap;

  // Binned bitmap index policy
  std::vector<uint32_t> bitmap_nums;  // # of bitmaps for each attr
  // ordered binning policy: vector<uint64_t> (okey thresholds)
  // unordered binning policy: vector<pair<string,uint32_t>>
  std::vector<std::variant<std::vector<uint64_t>,
                           std::vector<std::pair<std::string, uint32_t>>>>
      binning_policy;
};
class SABIBuilder;
class SABIUDIIterator;
class SABIReader;
class SABIFactory;

// Per-SST histogram of one ORDERED attribute, the raw material for DB-level
// cardinality estimation: bin i covers okeys [boundaries[i], boundaries[i+1])
// (the last bin also includes its upper edge), counts[i] is that bin's row
// count. NULL and tombstone rows sit in no value bin, so they are excluded by
// construction.
struct OrderedAttrHistogram {
  std::vector<uint64_t> boundaries;  // absolute okeys, bin_count + 1
  std::vector<uint64_t> counts;      // bin_count
  // Exact distinct okeys in this SST (v6+ blobs; 0 = unknown/v5). Feeds the
  // estimator's equality floor on sparse integer domains.
  uint64_t distinct = 0;
};

// Per-SST equality-count material for one UNORDERED attribute: every
// interned distinct value paired with an estimated row count, sorted by
// value. The blob persists only per-bin bitmaps, so a value alone in its bin
// is exact and values sharing a bin split the bin's cardinality uniformly.
// NULL and tombstone rows sit in no value bin, so they are excluded by
// construction.
struct UnorderedAttrValueCounts {
  std::vector<std::pair<std::string, double>> value_counts;
};

class SABIBuilder : public rocksdb::UserDefinedIndexBuilder {
 private:
  SABISchema schema_;
  std::unique_ptr<AttrExtractor> extractor_;  // exclusively owned
  std::vector<EncodedAttr>
      scratch_;  // per-row extraction buffer, attr_num slots

  // Interned buffer for one unordered attribute: each distinct value is
  // appended once to a string arena and rows keep only its dense id. The
  // value -> id lookup is a flat open-addressing table so high-cardinality
  // attributes pay no per-value node allocation.
  struct CatAttrBuf {
    struct ValueRef {
      uint32_t offset;
      uint32_t len;
    };
    std::string arena;  // concatenated distinct values
    std::vector<ValueRef> value_by_id;
    std::vector<uint32_t> count_by_id;
    std::vector<uint32_t> row_ids;    // row -> id
    std::vector<uint32_t> bin_by_id;  // id -> bin, set by binning policy

    std::string_view ValueOf(uint32_t id) const {
      const ValueRef& v = value_by_id[id];
      return std::string_view(arena.data() + v.offset, v.len);
    }
    void Intern(std::string_view value);

   private:
    static constexpr size_t kInitSlots = 1024;  // power of two
    std::vector<uint64_t> slot_hash_ = std::vector<uint64_t>(kInitSlots);
    std::vector<uint32_t> slot_id_ =
        std::vector<uint32_t>(kInitSlots, 0);  // id + 1; 0 = empty slot
    size_t used_ = 0;

    void Grow();
  };

  // Per-attr value buffer, dense: data rows only (ORDERED okeys, 8B each;
  // UNORDERED interned bytes). NULL and tombstone rows push nothing — they
  // live solely in attr_null_rows_ / tombstone_bitmap — so binning statistics
  // are plain scans. CalculateBitmapIndex is the single place that re-aligns
  // buffer entries with row ids by skipping exactly those bitmaps' ids.
  std::vector<std::variant<CatAttrBuf, std::vector<uint64_t>>> attr_buf_;
  // Per-attr set of row ids whose value is SQL NULL. NULL rows land in no
  // value bin and never enter binning-boundary estimation, so range/equality
  // queries auto-exclude them. Empty for non-nullable attrs.
  std::vector<roaring::Roaring> attr_null_rows_;

  // Statistics
  uint64_t total_data_entries_size_uncomp_ = 0;  // total size of KVPs (bytes)
  uint32_t data_entries_cnt_ = 0;   // total number of KVPs in current table
  uint32_t index_entries_cnt_ = 0;  // total number of index entries added
  // Per-attr exact distinct values (v6 directory field). ORDERED: counted in
  // SetOrderedPropertyBinningPolicy; UNORDERED: the interning table size.
  std::vector<uint64_t> distinct_cnts_;

  // Bitmap Index
  BitmapIndex bitmap_index_;

  // Index blob
  std::string index_blob_;

  // Captured by Finish() for tests only: each bin's (tombstone last)
  // getFrozenSizeInBytes(), read right before writeFrozen() serializes it.
  std::vector<uint32_t> last_finish_bitmap_sizes_;

  // Helper methods
  void SetBinningPolicy();
  void SetUnorderedPropertyBinningPolicy(uint32_t i);
  void SetOrderedPropertyBinningPolicy(uint32_t i);
  void CalculateBitmapIndex();

 public:
  SABIBuilder(SABISchema schema, std::unique_ptr<AttrExtractor> extractor);
  rocksdb::Slice AddIndexEntry(
      const rocksdb::Slice& last_key_in_current_block,
      const rocksdb::Slice* first_key_in_next_block,
      const rocksdb::UserDefinedIndexBuilder::BlockHandle& block_handle,
      std::string* separator_scratch);
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value);
  rocksdb::Status Finish(rocksdb::Slice* index_contents);
  // Test-only: exact per-bin (tombstone last) frozen sizes as measured by
  // Finish() immediately before writeFrozen() -- ground truth independent of
  // anything a reader later derives from the persisted blob. Valid after
  // Finish() returns.
  const std::vector<uint32_t>& TEST_LastFinishBitmapSizes() const {
    return last_finish_bitmap_sizes_;
  }
  void Dump();
};

// Not used. It's only for implementing UDI interface
class SABIUDIIterator : public rocksdb::UserDefinedIndexIterator {
 public:
  SABIUDIIterator(const SABIReader* reader);
  void Prepare(const rocksdb::ScanOptions scan_opts[], size_t num_opts);
  rocksdb::Status SeekAndGetResult(const rocksdb::Slice& target,
                                   rocksdb::IterateResult* result);
  rocksdb::Status NextAndGetResult(rocksdb::IterateResult* result);
  rocksdb::UserDefinedIndexBuilder::BlockHandle value();
};

enum class SABIReaderMode { kResident, kMetadata };

// One decoded bin as a block-cache entry: the 32B-aligned bytes and the
// frozen view over them, built once on a miss and shared by every query
// while cached. buf is declared before view so the view dies first.
struct SABICachedBin {
  std::unique_ptr<char[], void (*)(void*)> buf{nullptr, std::free};
  roaring::Roaring view;
  size_t charge = 0;
};

// RAII pin: keeps one cached bin alive for as long as a scan borrows its
// view. `owned` is the fallback when a cache insert is refused (strict
// capacity): the bin then lives exactly as long as the pin.
struct SABIPinnedBin {
  rocksdb::Cache* cache = nullptr;
  rocksdb::Cache::Handle* handle = nullptr;
  std::unique_ptr<SABICachedBin> owned;
  const roaring::Roaring* view = nullptr;

  SABIPinnedBin() = default;
  SABIPinnedBin(SABIPinnedBin&& o) noexcept { *this = std::move(o); }
  SABIPinnedBin& operator=(SABIPinnedBin&& o) noexcept {
    Release();
    cache = o.cache;
    handle = o.handle;
    owned = std::move(o.owned);
    view = o.view;
    o.cache = nullptr;
    o.handle = nullptr;
    o.view = nullptr;
    return *this;
  }
  SABIPinnedBin(const SABIPinnedBin&) = delete;
  SABIPinnedBin& operator=(const SABIPinnedBin&) = delete;
  ~SABIPinnedBin() { Release(); }
  void Release() {
    if (cache != nullptr && handle != nullptr) cache->Release(handle);
    cache = nullptr;
    handle = nullptr;
    view = nullptr;
    owned.reset();
  }
};

// Process-wide totals for the on-demand bin cache; counted here because
// these reads bypass BlockBasedTable, so RocksDB's tickers never see them.
struct SABIBinCacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t bytes_read = 0;  // bytes pulled off disk
  // read + decode events (the v1 design's smoking gun)
  uint64_t bitmaps_loaded = 0;
  // A strict/undersized cache refused a decoded-bin insert; the bin lived
  // only as long as its pin.
  uint64_t inserts_refused = 0;
};
SABIBinCacheStats GetSABIBinCacheStats();
void ResetSABIBinCacheStats();

class SABIReader : public rocksdb::UserDefinedIndexReader {
 private:
  SABISchema schema_;
  using AlignedPtr = std::unique_ptr<char[], void (*)(void*)>;
  std::vector<AlignedPtr> managed_buffers_;
  SABIReaderMode mode_ = SABIReaderMode::kResident;
  const rocksdb::BlockBasedTable* table_ = nullptr;
  // First index of attr_idx's bin range in the flat bitmap array.
  uint32_t AttrBinOffset(uint32_t attr_idx) const;

 public:
  // Self-describing: the schema residue (attr roles) is parsed from the
  // blob's directory, so no schema binding is needed to open an SST.
  explicit SABIReader(rocksdb::Slice& index_block);  // = kResident
  SABIReader(rocksdb::Slice& index_block, SABIReaderMode mode);
  const SABISchema& schema() const { return schema_; }
  BitmapIndex bitmap_index;
  std::vector<uint32_t> data_entries_cnt_psum;
  std::vector<rocksdb::BlockHandle> block_handles;
  // Per-attr exact distinct values (v6 blobs; all-zero for v5 = unknown).
  std::vector<uint64_t> distinct_cnts;
  // v7 blobs: per-bin cardinalities, tombstone last; empty on v5/v6.
  std::vector<uint32_t> bin_cardinalities;
  uint32_t TotalBins() const;
  // Persisted on v7; derived from the decoded bitmap on older blobs.
  uint64_t BinCardinality(uint32_t flat_idx) const;
  uint64_t TombstoneCardinality() const;
  // Bitmap extents: padded start offsets (total_bins+2) and exact frozen
  // sizes (total_bins+1; derived from offset differences on v5/v6).
  std::vector<uint32_t> bitmap_offsets_;
  std::vector<uint32_t> bitmap_sizes_;
  std::unique_ptr<rocksdb::UserDefinedIndexIterator> NewIterator(
      const rocksdb::ReadOptions& read_options);
  size_t ApproximateMemoryUsage() const;
  bool RetainsIndexContents() const override {
    return mode_ == SABIReaderMode::kResident;
  }
  void SetTable(const rocksdb::BlockBasedTable* table) override {
    table_ = table;
  }
  // Resident: a direct pointer, pin untouched. Metadata: cache lookup or
  // read+decode+insert; *pin keeps the bin alive; nullptr on I/O failure.
  // flat_idx == TotalBins() is the tombstone.
  const roaring::Roaring* Bin(uint32_t flat_idx, SABIPinnedBin* pin);
  // Returns false only when the query is provably unsatisfiable in this SST
  // (safe to skip all bitmap work and block fetches). Never returns false
  // for a query that could actually match a row.
  bool QueryCanMatch(const SABIQuery& q) const;
  // Fills `out` with attr_idx's histogram in absolute okey coordinates.
  // Returns false when the attr is out of range, not ORDERED, or has zero
  // binned rows (its stored boundaries are meaningless then).
  bool OrderedHistogram(uint32_t attr_idx, OrderedAttrHistogram* out) const;
  // Fills `out` with attr_idx's per-value counts (see UnorderedAttrValueCounts
  // for exactness). Returns false when the attr is out of range, not
  // UNORDERED, or has zero binned rows.
  bool UnorderedValueCounts(uint32_t attr_idx,
                            UnorderedAttrValueCounts* out) const;
  void Dump();
};

class SABIFactory : public rocksdb::UserDefinedIndexFactory {
 public:
  // Builds run concurrently, so each NewBuilder() gets a fresh extractor from
  // this callable; the callable itself must be thread-safe to invoke.
  using ExtractorFactory = std::function<std::unique_ptr<AttrExtractor>()>;

  // Reader-only factory: readers self-describe from the blob (v5+), so no
  // schema is needed to open SSTs. NewBuilder() is unavailable in this state.
  SABIFactory() = default;
  SABIFactory(SABISchema schema, ExtractorFactory extractor_factory)
      : schema_(std::move(schema)),
        extractor_factory_(std::move(extractor_factory)) {}
  // Standalone convenience: derives the schema residue and wires the default
  // v3-layout extractor. The only constructor that carries `options` itself
  // (ondemand_index gates the reader mode below); the other constructors
  // leave options_ default (ondemand_index == false).
  explicit SABIFactory(const BitLSMOptions& options)
      : SABIFactory(SABISchema::FromOptions(options), [options] {
          return std::make_unique<ValueLayoutExtractor>(options);
        }) {
    options_ = options;
  }
  const char* Name() const override;
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice& index_block_) const override;
  // Rejects blobs with a missing or unsupported version footer, an invalid
  // directory, or (when this factory is schema-bound) roles that differ from
  // the bound schema. A schema-less factory skips the roles cross-check and
  // trusts the blob's directory.
  rocksdb::Status NewReader(
      const rocksdb::UserDefinedIndexOption& option,
      rocksdb::Slice& index_block,
      std::unique_ptr<rocksdb::UserDefinedIndexReader>& reader) const override;
  // True exactly when options_.ondemand_index: every reader this factory
  // produces then has RetainsIndexContents() == false, so the open path can
  // skip caching the raw block it is about to discard.
  bool ProducesMetadataOnlyReaders() const override;

 private:
  SABISchema schema_;
  ExtractorFactory extractor_factory_;
  // Value-init: attr_num/rho/read_seqno have no in-class default member
  // initializers, so `{}` is what keeps them zero (rather than
  // indeterminate) for the schema-less/schema-bound constructors that never
  // assign options_.
  BitLSMOptions options_{};
};

}  // namespace bit_lsm
