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
#include "cache/cache_key.h"
#include "roaring.hh"
#include "rocksdb/advanced_cache.h"  // full rocksdb::Cache / Cache::Handle
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include "table/format.h"

namespace rocksdb {
class FilePrefetchBuffer;  // held by SABISpanPrefetch through unique_ptr only
}  // namespace rocksdb

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
class SABISpanPrefetch;

// One condition's flat-bin extent, [first, last] both inclusive; the
// tombstone is the single bin {TotalBins(), TotalBins()}. Produced by
// SABIReader::SelectBins, consumed by the bin loaders and the span-prefetch
// plan alike, so the two can never disagree about which bins a query needs.
struct BinSelection {
  uint32_t first;
  uint32_t last;
};

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

// One decoded bin as a block-cache entry: a frozen view over a shared
// 32B-aligned arena, built once on a miss and shared by every query while
// cached. Every bin decoded by one coalesced span read shares one arena
// (v7 32B-aligns each bitmap start, so each view is in place), which lives
// until the last sharing entry dies -- eviction order across a span never
// matters. arena is declared before view so the view dies first.
struct SABICachedBin {
  std::shared_ptr<char> arena;
  roaring::Roaring view;
  // The bin's PADDED extent + the entry struct: a span's entries then sum to
  // about their arena's size -- the buffer is charged once, neither doubled
  // nor dropped. Approximate beyond that: the frozen view's own heap
  // metadata and the shared_ptr control block stay uncharged.
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
  // File reads issued: one per coalesced run of missed bins, so a cold range
  // predicate costs reads << bitmaps_loaded. Synchronous preads only:
  // async-served runs appear in spans_prefetched instead, and a
  // submitted-but-unconsumed span read appears in neither -- take device
  // bytes from OS counters, not bytes_read.
  uint64_t reads = 0;
  uint64_t bytes_read = 0;  // bytes pulled off disk
  // read + decode events (the v1 design's smoking gun)
  uint64_t bitmaps_loaded = 0;
  // A strict/undersized cache refused a decoded-bin insert; the bin lived
  // only as long as its pin.
  uint64_t inserts_refused = 0;
  // Cache-missing runs the span-prefetch plan computed, counted only when
  // the machinery engaged (>= 2 runs; see SABISpanPrefetch) -- so a build
  // with no liburing still shows the path ran before its submits were
  // refused.
  uint64_t spans_planned = 0;
  // Miss runs served out of a span-prefetch buffer instead of a fresh
  // pread; such a run adds bytes_read but no reads.
  uint64_t spans_prefetched = 0;
  // Planned runs the submit phase did not put in flight -- async reads
  // unavailable, the per-query slot cap, or a refused submit -- so their
  // loads take the sync pread path (visible in `reads`). Silent async
  // disablement shows up here as spans_dropped == spans_planned.
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
  // The table's block cache; null when caller-supplied table options disable
  // it (the probe/load paths then hand decoded bins straight to their pins).
  rocksdb::Cache* BinCache() const;
  // The block-cache key of flat_idx's decoded bin, derived from the file's
  // base key and the bin's absolute file offset; the single definition the
  // probe, load and peek paths all share.
  rocksdb::CacheKey BinCacheKey(uint32_t flat_idx) const;
  // Cache probe for one bin; on a hit fills *slot (pinning the entry) and
  // returns true. slot->view stays null on a miss.
  bool ProbeBin(rocksdb::Cache* cache, uint32_t flat_idx, SABIPinnedBin* slot);
  // Loads missed bins [a, b] (contiguous in the blob) with ONE file read --
  // or a copy out of `prefetch`'s buffers when they already hold the run --
  // into one shared 32B-aligned arena, freezes each bin's view in place,
  // inserts each into `cache` (pin owns the bin on refusal or null cache;
  // the arena stays shared either way) and fills slots[i - a]. Returns false
  // on allocation or I/O failure; nothing is cached or filled then.
  bool LoadRun(uint32_t a, uint32_t b, rocksdb::Cache* cache,
               SABIPinnedBin* slots, SABISpanPrefetch* prefetch);

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
  // flat_idx == TotalBins() is the tombstone. A non-null `prefetch` lets a
  // cold load copy out of an already-submitted span read instead of issuing
  // its own pread (SABISpanPrefetch).
  const roaring::Roaring* Bin(uint32_t flat_idx, SABIPinnedBin* pin,
                              SABISpanPrefetch* prefetch = nullptr);
  // Computes cond's flat-bin extent without loading anything: the single bin
  // an UNORDERED equality maps to, or the clamped [first, last] range an
  // ORDERED comparison covers. Returns false when the condition matches no
  // bin (the empty result). The one source of truth for bin selection: the
  // iterator's materialization and the span-prefetch plan both consume it.
  bool SelectBins(const SABICondition& cond, BinSelection* out) const;
  // True when flat_idx's decoded bin is in the block cache right now; takes
  // no pin and counts no hit/miss stats (the span-prefetch plan peeks ahead
  // of the real probes).
  bool BinCached(uint32_t flat_idx) const;
  // Span loader for one attr's contiguous bins [first_flat, last_flat]
  // (inclusive): probes the cache per bin, then coalesces each maximal run
  // of misses into ONE file read backing one shared arena. Views append to
  // *out_views in flat-idx order; *out_pins keeps them alive. Resident mode
  // appends direct pointers, pins untouched. Returns false on I/O failure
  // (treat like Bin() returning nullptr); appends nothing then. `prefetch`
  // as in Bin().
  bool BinRange(uint32_t first_flat, uint32_t last_flat,
                std::vector<const roaring::Roaring*>* out_views,
                std::vector<SABIPinnedBin>* out_pins,
                SABISpanPrefetch* prefetch = nullptr);
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

// Overlaps one query's cold bin reads on one SST (metadata mode only). Span
// coalescing already turns a range predicate's contiguous misses into one
// pread, but the k conditions' runs live in different attr regions of the
// blob -- far apart -- plus the tombstone, and BuildQueryBitmap would issue
// those (k+1) preads one at a time. This plans the cache-missing byte runs
// up front, submits each as one async read (block_prefetch_queue.h's
// per-slot FilePrefetchBuffer pattern), and lets LoadRun copy out of the
// buffers as it walks the conditions: cold-path latency drops from the sum
// of the run reads toward their max.
//
// Per-scan lifetime: created by the owning SABITableIterator and destroyed
// with it; each run's buffer is freed as soon as its bytes are fully
// consumed, so only unconsumed runs survive to the destructor. On any
// unavailability -- no liburing (the shared process-wide flag in
// block_prefetch_queue.h), a refused submit, a short buffer -- consumers
// silently read the ordinary way: results identical, speed unchanged.
class SABISpanPrefetch {
 public:
  // `reader` must be a metadata-mode reader of `table`; both must outlive
  // this object (the iterator's table pin covers that).
  SABISpanPrefetch(SABIReader* reader, const rocksdb::BlockBasedTable* table);
  // Out-of-line so FilePrefetchBuffer stays forward-declared here;
  // destruction aborts any read nobody consumed.
  ~SABISpanPrefetch();

  // Probes the cache over the selections' union, coalesces the missing bins
  // into maximal byte-contiguous runs (extents meeting at a shared bin
  // boundary merge, whatever attr they belong to; a gap of even one
  // unwanted bin keeps runs separate, so no dead bytes ride along), and
  // submits one async read per run -- only when there are at least two
  // (a lone run gains nothing over the sync read LoadRun issues anyway),
  // and at most kMaxSlots of them, in plan order. Runs it does not put in
  // flight count as spans_dropped and load synchronously.
  void PlanAndSubmit(const std::vector<BinSelection>& selections);

  // Copies file bytes [offset, offset + len) into dst if one submitted run
  // fully holds them: the first touch of a run polls its read at the run's
  // own offset (the only offset an explicit async submit answers to);
  // sub-ranges then serve from the populated buffer with no further I/O.
  // False = read the file the ordinary way; a failed or short buffer kills
  // its whole run, never serving part of it.
  bool TryConsume(uint64_t offset, size_t len, char* dst);

 private:
  // kDead covers both a failed slot and a fully-drained one: either way the
  // consumer preads.
  enum class SlotState { kSubmitted, kPopulated, kDead };
  // One in-flight run. Unlike BlockPrefetchQueue's fixed reused window,
  // slots are per-query and never reused. The query does NOT bound the run
  // count -- cache state does: a half-resident range fragments into one run
  // per cached/uncached alternation, hundreds at fine rho -- so submission
  // stops at kMaxSlots and each buffer is freed the moment its run is fully
  // consumed rather than at end of scan.
  struct Slot {
    std::unique_ptr<rocksdb::FilePrefetchBuffer> buf;
    uint64_t offset = 0;  // absolute file offset of the run
    size_t len = 0;
    // Unconsumed bytes; consumed sub-ranges are disjoint whenever the bin
    // cache is live, so hitting 0 means the whole run was copied out and
    // buf can be freed. (With no bin cache sub-ranges can overlap; the
    // clamped subtraction then only frees early, degrading to preads.)
    size_t remaining = 0;
    SlotState state = SlotState::kDead;
    // Valid once kPopulated: the run's bytes inside buf, stable afterwards
    // because nothing touches buf again.
    rocksdb::Slice data;
  };

  // Submitted slots per query. Each slot holds its whole run's bytes from
  // submit until consumption (or death), so this caps transient memory when
  // a half-resident range plans hundreds of tiny runs; runs beyond the cap
  // just keep their sync preads. 64 slots cover every plausible
  // all-cold CNF (one run per condition region plus the tombstone) with
  // room to spare.
  static constexpr size_t kMaxSlots = 64;

  // Submits flat-bin miss run [a, b] (inclusive); false = nothing in
  // flight, the run's loads stay synchronous.
  bool Submit(uint32_t a, uint32_t b);

  SABIReader* reader_;
  const rocksdb::BlockBasedTable* table_;
  std::vector<Slot> slots_;
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
