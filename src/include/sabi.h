#pragma once

#define TEST_CACHE_LINE_SIZE 64  // To avoid compile error

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "bit_lsm_option.h"
#include "bit_lsm_query.h"
#include "bit_lsm_utils.h"
#include "roaring.hh"
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include "table/format.h"

namespace bit_lsm {

// On-disk format version of a BitLSM SST: covers the SABI index blob and,
// since the same code version writes both, the SST's value encoding.
// Bump on any breaking change.
inline constexpr uint32_t kBitLSMFormatVersion = 2;
// Trailing magic marking versioned SABI blobs. Blobs written before
// versioning existed end in an offset field instead, so readers can reject
// them cleanly rather than misparse.
inline constexpr uint32_t kSABIFooterMagic = 0xB17B5AB1;

struct BitmapIndex {
  // Bitmap
  std::vector<roaring::Roaring> bitmaps;
  roaring::Roaring tombstone_bitmap;

  // Binned bitmap index policy
  std::vector<uint32_t> bitmap_nums;  // # of bitmaps for each attr
  // continuous binning policy: vector<double>
  // categorical binning policy: vector<pair<string,uint32_t>>
  std::vector<std::variant<std::vector<double>,
                           std::vector<std::pair<std::string, uint32_t>>>>
      binning_policy;
};
class SABIBuilder;
class SABIUDIIterator;
class SABIReader;
class SABIFactory;

class SABIBuilder : public rocksdb::UserDefinedIndexBuilder {
 private:
  BitLSMOptions options_;
  ValueLayout value_layout_;

  // Interned buffer for one categorical attribute: each distinct value is
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

  // Buffer
  std::vector<std::variant<CatAttrBuf, std::vector<double>>> attr_buf_;

  // Statistics
  uint64_t total_data_entries_size_uncomp_ = 0;  // total size of KVPs (bytes)
  uint32_t data_entries_cnt_ = 0;   // total number of KVPs in current table
  uint32_t index_entries_cnt_ = 0;  // total number of index entries added

  // Bitmap Index
  BitmapIndex bitmap_index_;

  // Index blob
  std::string index_blob_;

  // Helper methods
  void SetBinningPolicy();
  void SetCategoricalPropertyBinningPolicy(uint32_t i);
  void SetContinuousPropertyBinningPolicy(uint32_t i);
  void CalculateBitmapIndex();

 public:
  SABIBuilder(BitLSMOptions options);
  rocksdb::Slice AddIndexEntry(
      const rocksdb::Slice& last_key_in_current_block,
      const rocksdb::Slice* first_key_in_next_block,
      const rocksdb::UserDefinedIndexBuilder::BlockHandle& block_handle,
      std::string* separator_scratch);
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value);
  rocksdb::Status Finish(rocksdb::Slice* index_contents);
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

class SABIReader : public rocksdb::UserDefinedIndexReader {
 private:
  BitLSMOptions options_;
  using AlignedPtr = std::unique_ptr<char[], void (*)(void*)>;
  std::vector<AlignedPtr> managed_buffers_;

 public:
  SABIReader(rocksdb::Slice& index_block, BitLSMOptions options_);
  BitmapIndex bitmap_index;
  std::vector<uint32_t> data_entries_cnt_psum;
  std::vector<rocksdb::BlockHandle> block_handles;
  std::unique_ptr<rocksdb::UserDefinedIndexIterator> NewIterator(
      const rocksdb::ReadOptions& read_options);
  size_t ApproximateMemoryUsage() const;
  // Returns false only when the query is provably unsatisfiable in this SST
  // (safe to skip all bitmap work and block fetches). Never returns false
  // for a query that could actually match a row.
  bool QueryCanMatch(const BitLSMQuery& q, const BitLSMOptions& opts) const;
  void Dump();
};

class SABIFactory : public rocksdb::UserDefinedIndexFactory {
 private:
  BitLSMOptions options_;

 public:
  SABIFactory(BitLSMOptions options) : options_(options) {};
  const char* Name() const override;
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader> NewReader(
      rocksdb::Slice& index_block_) const override;
  // Validates the versioned footer before constructing a reader; returns
  // Corruption for pre-versioned or unsupported-version blobs.
  rocksdb::Status NewReader(
      const rocksdb::UserDefinedIndexOption& option,
      rocksdb::Slice& index_block,
      std::unique_ptr<rocksdb::UserDefinedIndexReader>& reader) const override;
};

}  // namespace bit_lsm
