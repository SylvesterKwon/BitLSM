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
#include "bytes_list.h"
#include "cache/cache_key.h"
#include "roaring.hh"
#include "rocksdb/advanced_cache.h"  // full rocksdb::Cache / Cache::Handle
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include "table/format.h"

namespace rocksdb {
class FilePrefetchBuffer;
}  // namespace rocksdb

namespace bit_lsm {

// On-disk format version. Blobs with any other version are rejected at open.
//
// SABI index blob wire layout, as written by SABIBuilder::Finish:
//   [index entries]     per block: [row-count psum u32][bh.offset u32]
//                       [bh.size u32]
//   [frozen bitmaps]    per-attr value bins in attr order, tombstone bitmap
//                       last. Every bitmap starts on a 32-byte boundary
//                       (blob-relative); gap bytes are zero.
//   [binning policies]  per attr:
//                       kRange:   BytesList of bin_count+1 boundaries
//                                  ([count u32][end u32 x count][bytes]),
//                                  bytes in SABI's memcmp domain
//                       kEquality: [entry_count u32] then
//                                  {varint-len string, bin u32} x entry_count
//   [directory]         [attr_num u32][index_type u8 x attr_num]
//                       [bin_count u32 x attr_num][index_entries_cnt u32]
//                       [distinct_cnt u64 x attr_num]
//                       [bin_cardinality u32 x (total_bins+1)]
//                       [bitmap_size u32 x (total_bins+1)]
//                       [policy offset u32 x (attr_num+1)]
//                       [bitmap offset u32 x (total_bins+2)]
//   [footer]            [directory_off u32][version u32][magic u32]
inline constexpr uint32_t kBitLSMFormatVersion = 8;
// Trailing magic of the SABI blob footer
inline constexpr uint32_t kSABIFooterMagic =
    0x5AB1B175;  // SABIBITS in hexspeak :^)

struct BitmapIndex {
  // Bitmap
  std::vector<roaring::Roaring> bitmaps;
  roaring::Roaring tombstone_bitmap;

  // Binned bitmap index policy
  std::vector<uint32_t> bitmap_nums;  // # of bitmaps for each attr
  // ordered binning policy: BytesList (bin_count+1 boundaries, memcmp domain)
  // unordered binning policy: vector<pair<string,uint32_t>>
  std::vector<
      std::variant<BytesList, std::vector<std::pair<std::string, uint32_t>>>>
      binning_policy;
};
class SABIBuilder;
class SABIUDIIterator;
class SABIReader;
class SABIFactory;
class SABISpanPrefetch;

// One condition's flat-bin extent, [first, last] inclusive. The tombstone is
// bin TotalBins().
struct BinSelection {
  uint32_t first;
  uint32_t last;
};

// Per-SST histogram of one kRange attr: bin i covers [boundaries[i],
// boundaries[i+1]) in memcmp order (the last bin also includes its upper edge)
// and counts[i] is its row count. NULL and tombstone rows are in no bin.
struct RangeAttrHistogram {
  BytesList boundaries;          // bin_count + 1, SABI byte domain
  std::vector<uint64_t> counts;  // bin_count
  // Exact distinct values in this SST.
  uint64_t distinct = 0;
};

// Per-SST value counts of one kEquality attr, sorted by value. A value alone
// in its bin is exact; values sharing a bin split its cardinality uniformly.
// NULL and tombstone rows are in no bin.
struct EqualityAttrValueCounts {
  std::vector<std::pair<std::string, double>> value_counts;
};

class SABIBuilder : public rocksdb::UserDefinedIndexBuilder {
 private:
  SABISchema schema_;
  std::unique_ptr<AttrExtractor> extractor_;  // exclusively owned
  std::vector<EncodedAttr>
      scratch_;  // per-row extraction buffer, attr_num slots

  // Interned buffer for one kEquality attr: each distinct value is stored
  // once in an arena and rows keep its dense id. The value -> id lookup is a
  // flat open-addressing table.
  struct EqualityAttrBuf {
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

  // Per-row value bytes of one kRange attr, append only. Sort() fills
  // `sorted` and `distinct`.
  struct RangeAttrBuf {
    BytesList values;
    std::vector<uint32_t> sorted;     // row positions in value order
    std::vector<uint32_t> run_start;  // equal-value runs in `sorted`, plus n
    uint64_t distinct = 0;
    // Length shared by every value: -2 while empty, -1 once two lengths
    // differ. Uniform 8-byte values sort as contiguous okeys.
    int32_t uniform_len = -2;
    // Row -> local bin, filled by SetRangeBinningPolicy.
    std::vector<uint32_t> bin_of_row;
    void Push(std::string_view v);
    void Sort();
  };

  // Per-attr value buffer, data rows only: NULL and tombstone rows push
  // nothing.
  std::vector<std::variant<EqualityAttrBuf, RangeAttrBuf>> attr_buf_;
  // Per-attr row ids whose value is NULL. Empty for non-nullable attrs.
  std::vector<roaring::Roaring> attr_null_rows_;

  // Statistics
  uint64_t total_data_entries_size_uncomp_ = 0;  // total size of KVPs (bytes)
  uint32_t data_entries_cnt_ = 0;   // total number of KVPs in current table
  uint32_t index_entries_cnt_ = 0;  // total number of index entries added
  // Per-attr exact distinct values.
  std::vector<uint64_t> distinct_cnts_;

  // Bitmap Index
  BitmapIndex bitmap_index_;

  // Index blob
  std::string index_blob_;

  // Test-only: each bin's frozen size (tombstone last), captured by Finish().
  std::vector<uint32_t> last_finish_bitmap_sizes_;

  // Helper methods
  void SetBinningPolicy();
  void SetEqualityBinningPolicy(uint32_t i);
  void SetRangeBinningPolicy(uint32_t i);
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
  // Test-only; valid after Finish().
  const std::vector<uint32_t>& TEST_LastFinishBitmapSizes() const {
    return last_finish_bitmap_sizes_;
  }
  void Dump();
};

// Unused; satisfies the UDI interface.
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

// One decoded bin as a block-cache entry: a frozen view over a 32B-aligned
// arena shared by every bin loaded in the same run. The arena lives until the
// last sharing entry dies; it is declared before view so the view dies first.
struct SABICachedBin {
  std::shared_ptr<char> arena;
  roaring::Roaring view;
  // Cache charge: the bin's padded extent plus the entry struct.
  size_t charge = 0;
};

// RAII pin keeping one cached bin alive while a scan borrows its view.
// `owned` holds the bin when the cache refused the insert.
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

// Process-wide totals for the on-demand bin cache. These reads bypass
// BlockBasedTable, so RocksDB's tickers never see them.
struct SABIBinCacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  // Synchronous file reads issued, one per coalesced run of missed bins.
  // Async-served runs count in spans_prefetched instead.
  uint64_t reads = 0;
  uint64_t bytes_read = 0;  // bytes pulled off disk
  // bin read + decode events
  uint64_t bitmaps_loaded = 0;
  // Decoded-bin inserts the cache refused.
  uint64_t inserts_refused = 0;
  // Miss runs the span-prefetch plan computed, counted only when it engaged
  // (>= 2 runs).
  uint64_t spans_planned = 0;
  // Miss runs served from a span-prefetch buffer; they add bytes_read but no
  // reads.
  uint64_t spans_prefetched = 0;
  // Planned runs never put in flight (async reads unavailable, slot cap,
  // refused submit); they load synchronously.
  uint64_t spans_dropped = 0;
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
  // The table's block cache; null when the table options disable it.
  rocksdb::Cache* BinCache() const;
  // Block-cache key of flat_idx's decoded bin: the file's base key plus the
  // bin's file offset.
  rocksdb::CacheKey BinCacheKey(uint32_t flat_idx) const;
  // Cache probe for one bin: on a hit pins the entry into *slot and returns
  // true.
  bool ProbeBin(rocksdb::Cache* cache, uint32_t flat_idx, SABIPinnedBin* slot);
  // Loads missed bins [a, b] with one file read (or from `prefetch` when it
  // holds the run) into one shared arena, inserts each bin into `cache` and
  // fills slots[i - a]. Returns false on allocation or I/O failure; nothing
  // is cached or filled then.
  bool LoadRun(uint32_t a, uint32_t b, rocksdb::Cache* cache,
               SABIPinnedBin* slots, SABISpanPrefetch* prefetch);

 public:
  // The schema is parsed from the blob's directory; no binding needed.
  explicit SABIReader(rocksdb::Slice& index_block);  // = kResident
  SABIReader(rocksdb::Slice& index_block, SABIReaderMode mode);
  const SABISchema& schema() const { return schema_; }
  BitmapIndex bitmap_index;
  std::vector<uint32_t> data_entries_cnt_psum;
  std::vector<rocksdb::BlockHandle> block_handles;
  // Per-attr exact distinct values.
  std::vector<uint64_t> distinct_cnts;
  // Per-bin cardinalities, tombstone last.
  std::vector<uint32_t> bin_cardinalities;
  uint32_t TotalBins() const;
  uint64_t BinCardinality(uint32_t flat_idx) const;
  uint64_t TombstoneCardinality() const;
  // Bitmap extents: padded start offsets (total_bins+2) and exact frozen
  // sizes (total_bins+1).
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
  // Resident mode: a direct pointer, pin untouched. Metadata mode: cache
  // lookup or read+decode+insert, *pin keeps the bin alive, nullptr on I/O
  // failure. flat_idx == TotalBins() is the tombstone. A cold load copies
  // from `prefetch` when it holds the run.
  const roaring::Roaring* Bin(uint32_t flat_idx, SABIPinnedBin* pin,
                              SABISpanPrefetch* prefetch = nullptr);
  // cond's flat-bin extent, without loading anything. Returns false when the
  // condition matches no bin.
  bool SelectBins(const SABICondition& cond, BinSelection* out) const;
  // True when flat_idx's decoded bin is in the block cache; takes no pin and
  // counts no stats.
  bool BinCached(uint32_t flat_idx) const;
  // Loads contiguous bins [first_flat, last_flat]: probes the cache per bin
  // and coalesces each run of misses into one file read. Views append to
  // *out_views in flat-idx order, *out_pins keeps them alive. Returns false
  // on I/O failure and appends nothing then.
  bool BinRange(uint32_t first_flat, uint32_t last_flat,
                std::vector<const roaring::Roaring*>* out_views,
                std::vector<SABIPinnedBin>* out_pins,
                SABISpanPrefetch* prefetch = nullptr);
  // False only when the query provably matches no row of this SST.
  bool QueryCanMatch(const SABIQuery& q) const;
  // Fills `out` with attr_idx's histogram. Returns false when the attr is
  // out of range, not kRange, or has no binned rows.
  bool RangeHistogram(uint32_t attr_idx, RangeAttrHistogram* out) const;
  // Fills `out` with attr_idx's per-value counts. Returns false when the attr
  // is out of range, not kEquality, or has no binned rows.
  bool EqualityValueCounts(uint32_t attr_idx,
                           EqualityAttrValueCounts* out) const;
  void Dump();
};

// Overlaps one query's cold bin reads on one SST (metadata mode only). The
// conditions' miss runs live in different regions of the blob, so instead of
// one pread per run this submits every run as an async read up front and lets
// LoadRun copy out of the buffers. Per-scan lifetime; a run's buffer is freed
// once fully consumed. When async reads are unavailable or a submit is
// refused, consumers read the ordinary way.
class SABISpanPrefetch {
 public:
  // `reader` must be a metadata-mode reader of `table`; both must outlive
  // this object.
  SABISpanPrefetch(SABIReader* reader, const rocksdb::BlockBasedTable* table);
  // Out of line to keep FilePrefetchBuffer forward-declared; aborts any
  // unconsumed read.
  ~SABISpanPrefetch();

  // Probes the cache over the selections, coalesces the missing bins into
  // byte-contiguous runs and submits one async read per run, only when there
  // are at least two and at most kMaxSlots of them. Runs not put in flight
  // count as spans_dropped and load synchronously.
  void PlanAndSubmit(const std::vector<BinSelection>& selections);

  // Copies file bytes [offset, offset + len) into dst if one submitted run
  // fully holds them. False means read the file the ordinary way; a failed or
  // short buffer kills its whole run.
  bool TryConsume(uint64_t offset, size_t len, char* dst);

 private:
  // kDead: failed or fully drained; the consumer preads.
  enum class SlotState { kSubmitted, kPopulated, kDead };
  // One in-flight run; slots are per-query and never reused.
  struct Slot {
    std::unique_ptr<rocksdb::FilePrefetchBuffer> buf;
    uint64_t offset = 0;  // absolute file offset of the run
    size_t len = 0;
    // Unconsumed bytes; at 0 the whole run was copied out and buf is freed.
    size_t remaining = 0;
    SlotState state = SlotState::kDead;
    // The run's bytes inside buf; valid once kPopulated.
    rocksdb::Slice data;
  };

  // Cap on submitted slots per query; runs beyond it load synchronously.
  static constexpr size_t kMaxSlots = 64;

  // Submits miss run [a, b]; false means nothing in flight.
  bool Submit(uint32_t a, uint32_t b);

  SABIReader* reader_;
  const rocksdb::BlockBasedTable* table_;
  std::vector<Slot> slots_;
};

class SABIFactory : public rocksdb::UserDefinedIndexFactory {
 public:
  // Each NewBuilder() gets a fresh extractor from this callable, which must
  // be thread-safe: builds run concurrently.
  using ExtractorFactory = std::function<std::unique_ptr<AttrExtractor>()>;

  // Reader-only factory; NewBuilder() is unavailable.
  SABIFactory() = default;
  SABIFactory(SABISchema schema, ExtractorFactory extractor_factory)
      : schema_(std::move(schema)),
        extractor_factory_(std::move(extractor_factory)) {}
  // Standalone convenience: derives the schema and uses the ValueLayout
  // extractor. The only constructor that sets options_, which gates the
  // reader mode.
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
  // Rejects blobs with a missing or wrong version footer, an invalid
  // directory, or (when schema-bound) index_types that differ from the bound
  // schema.
  rocksdb::Status NewReader(
      const rocksdb::UserDefinedIndexOption& option,
      rocksdb::Slice& index_block,
      std::unique_ptr<rocksdb::UserDefinedIndexReader>& reader) const override;
  // True when options_.ondemand_index, so the open path can skip caching the
  // raw index block.
  bool ProducesMetadataOnlyReaders() const override;

 private:
  SABISchema schema_;
  ExtractorFactory extractor_factory_;
  BitLSMOptions options_{};
};

}  // namespace bit_lsm
