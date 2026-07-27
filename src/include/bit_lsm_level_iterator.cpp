#include <bit_lsm_iterator.h>
#include <bit_lsm_query.h>

#include <cstdint>
#include <iostream>

#include "db/column_family.h"
#include "db/version_set.h"
#include "rocksdb/options.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;
using namespace roaring;

BitLSMLevelIterator::BitLSMLevelIterator(uint32_t level,
                                         const ScanContext& scan_ctx,
                                         const QueryContext& query_ctx)
    : level_(level),
      scan_ctx_(scan_ctx),
      query_ctx_(query_ctx),
      files_(scan_ctx.storage_info->LevelFiles(level_)),
      cur_file_idx_(0),
      cur_table_handle_(nullptr),
      cur_sti_(nullptr) {}

BitLSMLevelIterator::~BitLSMLevelIterator() {
  // cur_sti_ holds a raw BlockBasedTable pointer and a pinned data block, so
  // it must be destroyed before the table cache handle that keeps that table
  // reader alive is released.
  if (cur_sti_) delete cur_sti_;
  if (cur_table_handle_) scan_ctx_.tc->get_cache().Release(cur_table_handle_);
}

void BitLSMLevelIterator::LoadFile(size_t idx) {
  // cout << "[BitLSMLevelIterator] level " << level_ << ", " << idx
  //      << " th file is loading\n";

  // 1. Clean up existing iterator & table handle (iterator first: it holds a
  // raw BlockBasedTable pointer and a pinned data block from that table)
  valid_ = false;
  TableCache::CacheInterface cache_interface = scan_ctx_.tc->get_cache();
  if (cur_sti_ != nullptr) {
    delete cur_sti_;
    cur_sti_ = nullptr;
  }
  if (cur_table_handle_ != nullptr) {
    cache_interface.Release(cur_table_handle_);
    cur_table_handle_ = nullptr;
  }

  // 2. Validate file index range
  if (idx >= files_.size()) {
    return;
  }

  // 3. Read BlockBasedTable
  const ReadOptions& read_options = ReadOptions();
  const FileOptions& file_options = FileOptions();
  TableCache::TypedHandle* new_table_handle = nullptr;
  const FileMetaData* file_meta = files_[idx];
  const bool no_io = false;

  Status s = scan_ctx_.tc->FindTable(
      read_options, file_options, *scan_ctx_.icmp, *file_meta,
      &new_table_handle, scan_ctx_.cf_opts, no_io);
  if (!s.ok()) {
    std::cerr << "Failed to load SST\n";
    return;
  }
  TableReader* table = cache_interface.Value(new_table_handle);
  BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);

  // 4. Prepare new SABITableIterator
  cur_table_handle_ = new_table_handle;
  cur_sti_ = new SABITableIterator(bbt, query_ctx_);
}

void BitLSMLevelIterator::SeekToFirst() {
  // 1. Set file cursor to zero
  cur_file_idx_ = 0;

  // 2. Load files sequentially until valid data is found
  while (cur_file_idx_ < files_.size()) {
    LoadFile(cur_file_idx_);

    if (cur_sti_ != nullptr) {
      cur_sti_->SeekToFirst();
      if (cur_sti_->Valid()) {
        valid_ = true;
        return;
      }
    }
    // If no valid data found, move next file
    cur_file_idx_++;
  }
  // If there's no valid data at all, valid_ is set to false
}

void BitLSMLevelIterator::Next() {
  // 1. Check current validity
  assert(Valid());

  // 2. Advance current SABITableIterator
  cur_sti_->Next();

  // 3. If current table is no longer valid, move to next valid table
  if (!cur_sti_->Valid()) {
    valid_ = false;
    while (true) {
      cur_file_idx_++;
      if (cur_file_idx_ >= files_.size()) {
        return;
      }
      LoadFile(cur_file_idx_);
      if (cur_sti_ != nullptr) {
        cur_sti_->SeekToFirst();
        if (cur_sti_->Valid()) {
          valid_ = true;
          return;
        }
      }
    }
  }
}

Slice BitLSMLevelIterator::key() const {
  assert(Valid());
  return cur_sti_->key();
}

Slice BitLSMLevelIterator::value() const {
  assert(Valid());
  return cur_sti_->value();
}