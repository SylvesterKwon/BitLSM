#include <bit_lsm_iterator.h>
#include <bit_lsm_query.h>

#include <iostream>

#include "db/column_family.h"
#include "db/memtable.h"
#include "db/version_set.h"
#include "rocksdb/options.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

BitLSMMergingIterator::BitLSMMergingIterator(const ScanContext& scan_ctx,
                                             const QueryContext& query_ctx)
    : scan_ctx_(scan_ctx), heap_(IteratorComparator(scan_ctx.icmp)) {
  // 1. Add MemTable iterators
  // 1-1. Active MemTable
  rocksdb::ReadOnlyMemTable* active_mem = scan_ctx_.sv->mem;
  if (active_mem != nullptr) {
    rocksdb::MemTable* mem = static_cast<rocksdb::MemTable*>(active_mem);
    ch_iters_.push_back(new BitLSMMemTableIterator(mem, query_ctx));
  }
  // 1-2. Immutable MemTables
  MemTableListVersion* memtable_list_version_ = scan_ctx_.sv->imm;
  if (memtable_list_version_ != nullptr) {
    for (ReadOnlyMemTable* imm_mem : memtable_list_version_->GetMemTables()) {
      rocksdb::MemTable* mem = static_cast<rocksdb::MemTable*>(imm_mem);
      ch_iters_.push_back(new BitLSMMemTableIterator(mem, query_ctx));
    }
  }

  // 2. Add L0 iterators
  TableCache::CacheInterface cache_interface = scan_ctx_.tc->get_cache();
  const vector<FileMetaData*>& l0_files = scan_ctx_.storage_info->LevelFiles(0);
  for (FileMetaData* meta : l0_files) {
    TableCache::TypedHandle* table_handle = nullptr;
    ReadOptions ro;
    Status s = scan_ctx_.tc->FindTable(ro, scan_ctx_.file_opts, *scan_ctx_.icmp,
                                       *meta, &table_handle, scan_ctx_.cf_opts);
    if (!s.ok()) {
      // Skipping the file would silently drop every row it holds, so record
      // the failure; SeekToFirst() refuses to scan once status_ is non-OK.
      std::cerr << "[BitLSMMergingIterator]: Failed to load L0 file "
                << meta->fd.GetNumber() << ": " << s.ToString() << "\n";
      if (status_.ok()) status_ = s;
      continue;
    }
    TableReader* table = cache_interface.Value(table_handle);
    BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);

    // cout << "[BitLSMMergingIterator] Added L0 SST iterator\n";
    ch_iters_.push_back(new SABITableIterator(bbt, query_ctx,
                                              /*source_level=*/0,
                                              meta->fd.GetNumber()));
    l0_handles_.push_back(table_handle);
  }

  // 4. Add L1+ level iterators
  for (uint32_t level = 1;
       level < scan_ctx_.storage_info->num_non_empty_levels(); ++level) {
    if (scan_ctx_.storage_info->NumLevelFiles(level) > 0) {
      ch_iters_.push_back(new BitLSMLevelIterator(level, scan_ctx_, query_ctx));
    }
  }
};

BitLSMMergingIterator::~BitLSMMergingIterator() {
  // 1. Free children iterators (before releasing the handles below: the L0
  // SABITableIterators dereference a raw BlockBasedTable pointer, and their
  // udi_entry_/sabi_reader_ may alias memory owned by that table's Rep, both
  // only valid while the handle keeps the table reader alive)
  for (auto* ch : ch_iters_) delete ch;
  ch_iters_.clear();

  // 2. Release cache for L0 SST
  for (auto* handle : l0_handles_) scan_ctx_.tc->get_cache().Release(handle);
  l0_handles_.clear();
}

void BitLSMMergingIterator::SeekToFirst() {
  // 1. Clear existing heap
  while (!heap_.empty()) heap_.pop();
  valid_ = false;

  // 2. A child that failed to open (see the constructor) already lost rows:
  // never produce a partial merge over the survivors.
  if (!status_.ok()) return;

  // 3. Propagate SeekToFirst & Register to heap
  for (auto* child : ch_iters_) {
    child->SeekToFirst();
    if (child->Valid()) {
      heap_.push(child);
      continue;
    }
    // An invalid child is end-of-data only while its status is OK. The first
    // non-OK status wins and ends the whole merge: the rows behind it are
    // unknown, so any output here would be an incomplete result.
    if (!child->status().ok()) {
      status_ = child->status();
      while (!heap_.empty()) heap_.pop();
      return;
    }
  }

  // 4. Set current iterator
  if (!heap_.empty()) {
    valid_ = true;
  }
}

void BitLSMMergingIterator::Next() {
  assert(Valid());
  // cout << "[BitLSMMergingIterator] Next() Called\n";

  // 1. Forward top iterator in heap
  SABIInternalIterator* top_iter = heap_.top();
  heap_.pop();
  top_iter->Next();
  if (top_iter->Valid()) {
    heap_.push(top_iter);
  } else if (!top_iter->status().ok()) {
    // The child stopped on an error rather than running out of rows; the
    // merge cannot complete, so stop instead of draining the other children.
    status_ = top_iter->status();
    while (!heap_.empty()) heap_.pop();
    valid_ = false;
    return;
  }

  // 2. Update valid_
  valid_ = !heap_.empty();
}

Slice BitLSMMergingIterator::key() const {
  assert(Valid());
  return heap_.top()->key();
}

Slice BitLSMMergingIterator::value() const {
  assert(Valid());
  return heap_.top()->value();
}
