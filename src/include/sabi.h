#pragma once

#define TEST_CACHE_LINE_SIZE 64 // To avoid compile error

#include "bit_lsm_option.h"
#include "roaring.hh"
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include "table/format.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace bit_lsm {

struct BitmapIndex {
  // Bitmap
  std::vector<roaring::Roaring> bitmaps;
  roaring::Roaring tombstone_bitmap;

  // Binned bitmap index policy
  std::vector<uint32_t> bitmap_nums; // # of bitmaps for each attr
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

  // Buffer
  std::vector<std::variant<std::vector<std::string>, std::vector<double>>>
      attr_buf_;

  // Statistics
  uint64_t total_data_entries_size_uncomp_ = 0; // total size of KVPs (bytes)
  uint32_t data_entries_cnt_ = 0;  // total number of KVPs in current table
  uint32_t index_entries_cnt_ = 0; // total number of index entries added

  // Bitmap Index
  BitmapIndex bitmap_index_;

  // Index blob
  std::string index_blob_;

  // Helper methods
  void SetBinningPolicy();
  void SetCategoricalPropertyBinningPolicy(
      uint32_t i,
      std::vector<std::unordered_map<std::string_view, uint32_t>>& cat_buf_map);
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
  std::unique_ptr<rocksdb::UserDefinedIndexIterator>
  NewIterator(const rocksdb::ReadOptions& read_options);
  size_t ApproximateMemoryUsage() const;
  void Dump();
};

class SABIFactory : public rocksdb::UserDefinedIndexFactory {
private:
  BitLSMOptions options_;

public:
  SABIFactory(BitLSMOptions options) : options_(options) {};
  const char* Name() const override;
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader>
  NewReader(rocksdb::Slice& index_block_) const override;
};

} // namespace bit_lsm
