#pragma once

#include "roaring.hh"
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace bitmap_index {

enum SKType {
  CATEGORICAL,
  CONTINUOUS,
};
struct SABIOptions {
  uint32_t sk_num;              // # of secondary keys
  std::vector<SKType> sk_types; // sk type vector
  double rho; // proportion parameter that determines bitmap budget
};
class SABIBuilder;
class SABIIterator;
class SABIReader;
class SABIFactory;

class SABIBuilder : public rocksdb::UserDefinedIndexBuilder {
private:
  SABIOptions options_;

  // Buffer
  std::vector<std::vector<std::string>> sk_buf_;

  // Statistics
  uint32_t total_data_entries_size_uncomp_ = 0; // total size of KVPs (bytes)
  uint32_t data_entries_cnt_ = 0;  // total number of KVPs in current table
  uint32_t index_entries_cnt_ = 0; // total number of index entries added

  // Bitmap
  std::vector<roaring::Roaring> bitmap_index_;

  // Binned bitmap index policy
  uint32_t total_bitmap_index_num_ = 0;     // total number of bitmap indexes
  std::vector<uint32_t> bitmap_index_nums_; // # of bitmaps for each SK
  // continuous binning policy: vector<double>
  // categorical binning policy: vector<pair<string,uint32_t>>
  std::vector<std::variant<std::vector<double>,
                           std::vector<std::pair<std::string, uint32_t>>>>
      binning_policy;

  // Index blob
  std::string index_blob_;

  // Helper methods
  void SetBinningPolicy();
  void SetCategoricalPropertyBinningPolicy(
      uint32_t i, std::vector<std::map<std::string_view, uint32_t>>& buf_map);
  void SetContinuousPropertyBinningPolicy(
      uint32_t i, std::vector<std::map<std::string_view, uint32_t>>& buf_map);
  void CalculateBitmapIndex();

public:
  SABIBuilder(SABIOptions options)
      : options_(options), sk_buf_(options.sk_num),
        bitmap_index_nums_(options.sk_num) {};
  rocksdb::Slice AddIndexEntry(const rocksdb::Slice& last_key_in_current_block,
                               const rocksdb::Slice* first_key_in_next_block,
                               const BlockHandle& block_handle,
                               std::string* separator_scratch);
  void OnKeyAdded(const rocksdb::Slice& key, ValueType type,
                  const rocksdb::Slice& value);
  rocksdb::Status Finish(rocksdb::Slice* index_contents);
};

class SABIIterator : public rocksdb::UserDefinedIndexIterator {
private:
  const SABIReader* reader_;
  const rocksdb::RangeOpt* range_;
  std::string qc; // TODO: support complex query condition
  std::vector<rocksdb::UserDefinedIndexBuilder::BlockHandle>
      block_handles_to_visit_;

public:
  SABIIterator(const SABIReader* reader);
  void Prepare(const rocksdb::ScanOptions scan_opts[], size_t num_opts);
  rocksdb::Status SeekAndGetResult(const rocksdb::Slice& target,
                                   rocksdb::IterateResult* result);
  rocksdb::Status NextAndGetResult(rocksdb::IterateResult* result);
  rocksdb::UserDefinedIndexBuilder::BlockHandle value();
};

struct SABIBlockIndexEntry {
  std::string index_key;
  rocksdb::UserDefinedIndexBuilder::BlockHandle block_handle;
  uint32_t prefix_kv_cnt;
};

class SABIReader : public rocksdb::UserDefinedIndexReader {
  friend class SABIIterator;

private:
  std::vector<roaring::Roaring> bitmap_index_;
  std::vector<SABIBlockIndexEntry> block_indices;
  using AlignedPtr = std::unique_ptr<char[], void (*)(void*)>;
  std::vector<AlignedPtr> managed_buffers_;

public:
  SABIReader(rocksdb::Slice& index_block);
  std::unique_ptr<rocksdb::UserDefinedIndexIterator>
  NewIterator(const rocksdb::ReadOptions& read_options);
  size_t ApproximateMemoryUsage() const;
};

class SABIFactory : public rocksdb::UserDefinedIndexFactory {
private:
  SABIOptions options_;

public:
  SABIFactory(SABIOptions options) : options_(options) {};
  const char* Name() const override;
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader>
  NewReader(rocksdb::Slice& index_block_) const override;
};

} // namespace bitmap_index
