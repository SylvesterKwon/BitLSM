#pragma once

#include "roaring.hh"
#include "rocksdb/options.h"
#include "rocksdb/user_defined_index.h"
#include <cstddef>
#include <memory>

namespace bitmap_index {

class SABIBuilder;
class SABIIterator;
class SABIReader;
class SABIFactory;

class SABIBuilder : public rocksdb::UserDefinedIndexBuilder {
private:
  uint32_t cur_table_kv_cnt_ = 0;  // total number of KVPairs in current table
  uint32_t index_entries_cnt_ = 0; // total number of index entries added
  std::vector<roaring::Roaring> roaring_set_;
  std::string final_index_blob_;

public:
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
  std::vector<roaring::Roaring> roaring_set_;
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
public:
  const char* Name() const override;
  rocksdb::UserDefinedIndexBuilder* NewBuilder() const override;
  std::unique_ptr<rocksdb::UserDefinedIndexReader>
  NewReader(rocksdb::Slice& index_block_) const override;
};

} // namespace bitmap_index
