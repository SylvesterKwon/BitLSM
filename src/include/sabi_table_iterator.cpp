#include <bit_lsm_query.h>

#include <cstdint>
#include <iostream>
#include <memory>

#include "rocksdb/options.h"
#include "table/block_based/block.h"
#include "table/format.h"
#define TEST_CACHE_LINE_SIZE \
  64  // To avoid compile error when using roaring.hh &
      // block_based_table_reader.h together

#include <bit_lsm_iterator.h>

#include "roaring.hh"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/block_based_table_reader_impl.h"  // Required: provides NewDataBlockIterator<> template definition

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;
using namespace roaring;

// Shared empty bitmap for query results that match nothing; never mutated.
// Function-local static so use during static init/teardown stays safe.
static const roaring::Roaring& EmptyBitmap() {
  static const roaring::Roaring kEmptyBitmap;
  return kEmptyBitmap;
}

void SABITableIterator::GetAllByIndexesFromDataBlock(
    const BlockHandle& bh, vector<uint32_t>& indexes,
    vector<PinnableSlice>& out_keys, vector<Slice>& out_values) {
  Status s;
  // Grow-only: PinSelf below assigns into each slot's retained buffer, and
  // buffer_count_ -- never size() -- is the valid-entry count.
  if (out_keys.size() < indexes.size()) {
    out_keys.resize(indexes.size());
    out_values.resize(indexes.size());
  }
  // Before the biter_ reset, which unpins the block out_values borrow from.
  buffer_count_ = 0;

  // Standard iterator readahead (implicit auto mode: ReadOptions default
  // readahead_size == 0). The prefetcher only allocates its buffer after
  // enough sequential reads, so this call is cheap for sparse targets.
  ReadOptions read_options;
  block_prefetcher_.PrefetchIfNeeded(
      bbt_->get_rep(), bh, read_options.readahead_size,
      /*is_for_compaction=*/false, /*no_sequential_checking=*/false,
      read_options, /*readaheadsize_cb=*/nullptr,
      /*is_async_io_prefetch=*/false);

  // Delete previous block iterator to unpin previous block value
  DataBlockIter* new_biter = bbt_->NewDataBlockIterator<DataBlockIter>(
      read_options, bh, nullptr, BlockType::kData, nullptr, nullptr,
      block_prefetcher_.prefetch_buffer(), false, false, s, true);
  biter_.reset(new_biter);
  if (!s.ok()) {
    // The block the bitmap pointed at is unreadable. Its rows cannot be
    // skipped silently, so stop the scan here and let the status propagate.
    status_ = s;
    return;  // buffer_count_ stays 0
  }

  uint32_t cur_checkpoint =
      UINT32_MAX;  // UINT32_MAX means no valid checkpoint is used
  uint32_t cur_offset = 0;

  for (uint32_t i = 0; i < indexes.size(); i++) {
    uint32_t target_checkpoint = indexes[i] / block_restart_interval_;
    if (cur_checkpoint != target_checkpoint) {
      cur_checkpoint = target_checkpoint;
      cur_offset = 0;
      biter_->SeekToRestartPoint(cur_checkpoint);
      biter_->Next();  // need to call Next once to access real data
    }
    uint32_t target_offset = indexes[i] % block_restart_interval_;
    while (cur_offset < target_offset && biter_->Valid()) {
      biter_->Next();
      cur_offset++;
    }
    if (biter_->Valid()) {
      // since key use delta-encoding and bufffer will be overwritten, we can't
      // zero-copy it
      out_keys[i].PinSelf(biter_->key());
      out_values[i] = biter_->value();
    } else {
      assert(false);
    }
  }
  buffer_count_ = static_cast<int32_t>(indexes.size());
}

SABITableIterator::SABITableIterator(BlockBasedTable* bbt,
                                     const QueryContext& query_ctx)
    : options_(query_ctx.options),
      bbt_(bbt),
      query_(query_ctx.sabi_query),
      compiled_(query_ctx.compiled),
      block_prefetcher_(
          /*compaction_readahead_size=*/0,
          bbt->get_rep()->table_options.initial_auto_readahead_size),
      query_bitmap_(&EmptyBitmap()),
      bitmap_iter_(query_bitmap_->begin()),
      bitmap_end_(query_bitmap_->end()) {
  // 0. Holds the SABI entry for this iterator's lifetime: a block cache pin
  // when cache_index_and_filter_blocks is on (evictable after release), or
  // an unowned reference to the table-lifetime pin in Rep when off. Either
  // way valid only while the table reader stays alive, which covers every
  // bitmap borrowed from the reader below.
  Status s = bbt_->GetUserDefinedIndexReader(ReadOptions(), &udi_entry_);
  if (!s.ok()) {
    // Every failure here is fatal for the scan, NotFound (an SST carrying no
    // SABI block) included: the query path has no fallback scan, so skipping
    // this file would silently drop every row it holds. Record the status so
    // the parent iterators stop instead of reading it as "no matching rows".
    cerr << "Failed to load SABI: " << s.ToString() << "\n";
    status_ = s;
    return;  // stays !Valid(); SeekToFirst is a no-op
  }
  sabi_reader_ = static_cast<SABIReader*>(udi_entry_.GetValue()->reader());

  block_restart_interval_ =
      bbt->get_rep()->table_options.block_restart_interval;

  // Early exit: skip all bitmap work and block fetches if the query is
  // provably unsatisfiable against this SSTable's min/max boundaries.
  if (!sabi_reader_->QueryCanMatch(query_)) return;

  // 1. Build query bitmap
  BuildQueryBitmap(query_);
  bitmap_iter_ = query_bitmap_->begin();
  bitmap_end_ = query_bitmap_->end();

  // 2. Get target block handles (binary search)
  int64_t last_added_block_idx = -1;
  auto psum_begin = sabi_reader_->data_entries_cnt_psum.begin();
  auto psum_end = sabi_reader_->data_entries_cnt_psum.end();
  for (uint32_t target_idx : *query_bitmap_) {
    // cout << "target_idx: " << target_idx << "\n";
    // target_idx is 0-based index, psum array is 1-based count array
    // so upper_bound is always right
    auto it = std::upper_bound(psum_begin, psum_end, target_idx);
    if (it != psum_end) {
      int64_t block_idx =
          std::distance(sabi_reader_->data_entries_cnt_psum.begin(), it);
      if (block_idx != last_added_block_idx) {
        target_blocks_.push_back(
            {block_idx, sabi_reader_->block_handles[block_idx]});
        last_added_block_idx = block_idx;
        psum_begin = it;
      }
    } else {
      assert(false);
    }
  }
  // Debug block hit ratio
  // sabi_reader_->Dump();
  // cout << "\n";
  // cout << "total_bhs_size: " << sabi_reader_->data_entries_cnt_psum.size()
  //      << "\n";
  // cout << "target_blocks_size: " << target_blocks_.size() << "\n";
  // cout << "("
  //      << (double)target_blocks_.size() /
  //             sabi_reader_->data_entries_cnt_psum.size()
  //      << ")\n";
}

// Build bitmap for a single SABICondition (leaf node in CNF)
SABITableIterator::BitmapRef SABITableIterator::GetBitmapForSingleCondition(
    const SABICondition& cond, vector<const roaring::Roaring*>& buf) {
  const AttrRole cur_attr_type = sabi_reader_->schema().roles[cond.attr_idx];

  uint32_t bitmap_offset = 0;
  for (uint32_t i = 0; i < cond.attr_idx; ++i)
    bitmap_offset += sabi_reader_->bitmap_index.bitmap_nums[i];

  if (cur_attr_type == AttrRole::UNORDERED) {
    if (cond.op != CompareOp::EQUAL) assert(false);
    const string& value = cond.bytes;
    vector<pair<string, uint32_t>>& cur_attr_binning_policy =
        get<vector<pair<string, uint32_t>>>(
            sabi_reader_->bitmap_index.binning_policy[cond.attr_idx]);
    auto it = std::lower_bound(
        cur_attr_binning_policy.begin(), cur_attr_binning_policy.end(), value,
        [](const pair<string, uint32_t>& policy_entry, const string& val) {
          return policy_entry.first < val;
        });
    if (it != cur_attr_binning_policy.end() && it->first == value) {
      return {&sabi_reader_->bitmap_index.bitmaps[bitmap_offset + it->second],
              nullptr};
    }
    return {&EmptyBitmap(), nullptr};
  } else if (cur_attr_type == AttrRole::ORDERED) {
    uint64_t value = cond.okey;
    const vector<uint64_t>& boundaries = std::get<vector<uint64_t>>(
        sabi_reader_->bitmap_index.binning_policy[cond.attr_idx]);
    uint32_t num_bins = sabi_reader_->bitmap_index.bitmap_nums[cond.attr_idx];

    // Find bin index by value
    // upper_bound: value보다 큰 첫 번째 경계값의 위치
    auto it = std::upper_bound(boundaries.begin(), boundaries.end(), value);

    int32_t target_bin_idx;
    if (it == boundaries.begin()) {
      // Case A: Given value is smaller than leftmost bin (virtual bin -1)
      target_bin_idx = -1;
    } else if (it == boundaries.end()) {
      // Case B: value >= all boundaries -> last bin. The top bin is closed on
      // the right, matching how the builder bins the maximum value.
      target_bin_idx = static_cast<int32_t>(num_bins) - 1;
    } else {
      // Case C: Else
      target_bin_idx =
          static_cast<int32_t>(std::distance(boundaries.begin(), it)) - 1;
    }
    // The virtual bin -1 (Case A) keeps the range math branch-free: e.g. LESS
    // on a below-min value yields the inverted range [0,-1], i.e. the empty
    // set.

    // Set raw range based on operator
    int32_t start_bin, end_bin;
    switch (cond.op) {
      case CompareOp::EQUAL:
        start_bin = target_bin_idx;
        end_bin = target_bin_idx;
        break;
      case CompareOp::GREATER_EQUAL:  // >= value
      case CompareOp::GREATER:        // > value
        start_bin = target_bin_idx;
        end_bin = static_cast<int32_t>(num_bins) - 1;
        break;
      case CompareOp::LESS_EQUAL:  // <= value
      case CompareOp::LESS:        // < value
        start_bin = 0;
        end_bin = target_bin_idx;
        break;
      default:
        assert(false);
    }
    // Clamp range
    if (start_bin < 0) start_bin = 0;
    if (static_cast<int32_t>(num_bins) <= end_bin)
      end_bin = static_cast<int32_t>(num_bins) - 1;

    if (start_bin > end_bin) return {&EmptyBitmap(), nullptr};
    if (start_bin == end_bin) {
      return {&sabi_reader_->bitmap_index.bitmaps[bitmap_offset + start_bin],
              nullptr};
    }

    // Merge bitmap (OR)
    buf.clear();
    buf.reserve(end_bin - start_bin + 1);
    for (int32_t i = start_bin; i <= end_bin; ++i)
      buf.push_back(&(sabi_reader_->bitmap_index.bitmaps[bitmap_offset + i]));
    bitmap_pool_.emplace_back(
        roaring::Roaring::fastunion(buf.size(), buf.data()));
    return {&bitmap_pool_.back(), &bitmap_pool_.back()};
  }
  return {&EmptyBitmap(), nullptr};
}

// CNF bitmap evaluation: AND of OR clauses. Sets query_bitmap_, borrowing
// reader-owned bitmaps where possible and materializing into bitmap_pool_
// only when a union/intersection/tombstone-filter result must be computed.
void SABITableIterator::BuildQueryBitmap(const SABIQuery& query) {
  const roaring::Roaring& tombstone =
      sabi_reader_->bitmap_index.tombstone_bitmap;

  if (query.clause_groups.empty()) {
    // Empty query is treated as a full table scan.
    uint32_t total = sabi_reader_->data_entries_cnt_psum.empty()
                         ? 0
                         : sabi_reader_->data_entries_cnt_psum.back();
    bitmap_pool_.emplace_back();
    roaring::Roaring& result = bitmap_pool_.back();
    result.addRange(0, total);
    if (!tombstone.isEmpty()) result -= tombstone;
    query_bitmap_ = &result;
    return;
  }

  vector<const roaring::Roaring*> bitmap_ptrs_buf;
  const roaring::Roaring* acc = nullptr;  // accumulated AND of clauses
  roaring::Roaring* acc_mut = nullptr;    // non-null when acc is pool-owned

  for (const auto& clause : query.clause_groups) {
    // OR within clause: union bitmaps of all conditions
    BitmapRef clause_bm;
    if (clause.empty()) {
      // An empty OR clause is satisfiable by nothing, so the AND is empty.
      clause_bm = {&EmptyBitmap(), nullptr};
    } else if (clause.size() == 1) {
      clause_bm = GetBitmapForSingleCondition(clause[0], bitmap_ptrs_buf);
    } else {
      vector<const roaring::Roaring*> cond_ptrs;
      cond_ptrs.reserve(clause.size());
      for (const auto& cond : clause)
        cond_ptrs.push_back(
            GetBitmapForSingleCondition(cond, bitmap_ptrs_buf).ptr);
      bitmap_pool_.emplace_back(*cond_ptrs[0] | *cond_ptrs[1]);
      roaring::Roaring& clause_union = bitmap_pool_.back();
      for (size_t i = 2; i < cond_ptrs.size(); ++i)
        clause_union |= *cond_ptrs[i];
      clause_bm = {&clause_union, &clause_union};
    }

    // AND between clauses
    if (acc == nullptr) {
      acc = clause_bm.ptr;
      acc_mut = clause_bm.owned;
    } else if (acc_mut != nullptr) {
      *acc_mut &= *clause_bm.ptr;
    } else {
      bitmap_pool_.emplace_back(*acc & *clause_bm.ptr);
      acc_mut = &bitmap_pool_.back();
      acc = acc_mut;
    }

    if (acc->isEmpty()) {
      query_bitmap_ = &EmptyBitmap();
      return;
    }
  }

  // Filter tombstones
  if (!tombstone.isEmpty()) {
    if (acc_mut != nullptr) {
      *acc_mut -= tombstone;
    } else {
      bitmap_pool_.emplace_back(*acc - tombstone);
      acc = &bitmap_pool_.back();
    }
  }
  query_bitmap_ = acc;
}

void SABITableIterator::LoadNextBlock() {
  while (true) {
    cur_target_block_idx_++;
    // 1. If there's no block left to read, return it
    if (cur_target_block_idx_ >= target_blocks_.size()) {
      valid_ = false;
      return;
    }

    // 2. Global (SST) index -> Local (Block) ID
    auto& [cur_bh_idx, cur_bh] = target_blocks_[cur_target_block_idx_];
    uint32_t global_start_idx =
        (cur_bh_idx == 0) ? 0
                          : sabi_reader_->data_entries_cnt_psum[cur_bh_idx - 1];
    uint32_t global_end_idx = sabi_reader_->data_entries_cnt_psum[cur_bh_idx];

    local_indexes_.clear();
    while (bitmap_iter_ != bitmap_end_) {
      uint32_t global_id = *bitmap_iter_;
      if (global_id >= global_end_idx) break;
      if (global_id >= global_start_idx)
        local_indexes_.push_back(global_id - global_start_idx);

      bitmap_iter_++;
    }

    // 3. Handle no row matching row for current block
    if (local_indexes_.empty()) continue;  // Move to next block

    // 4. Load data block
    GetAllByIndexesFromDataBlock(cur_bh, local_indexes_, keys_buffer_,
                                 values_buffer_);
    if (!status_.ok()) {
      valid_ = false;
      return;
    }

    // 5. Validate & filter value (two-pointer filtering)
    Status s;
    int32_t valid_cursor = 0;
    for (int32_t i = 0; i < buffer_count_; ++i) {
      // 5-1. Skip corrupted key
      ParsedInternalKey ikey;
      s = rocksdb::ParseInternalKey(keys_buffer_[i], &ikey, false);
      if (!s.ok()) continue;

      // 5-2. MVCC filtering
      if (ikey.sequence > options_.read_seqno) {
        continue;
      }

      // 5-3. Filter tombstone
      if (ikey.type == rocksdb::kTypeDeletion ||
          ikey.type == rocksdb::kTypeSingleDeletion) {
        continue;
      }

      // 5-4. Filter query condition (nullptr compiled = consumer re-verifies)
      if (!compiled_ || compiled_->Eval(values_buffer_[i])) {
        if (i != valid_cursor) {
          // Copy, not move: a move empties slot i and re-allocates it on the
          // next block.
          keys_buffer_[valid_cursor].PinSelf(keys_buffer_[i]);
          values_buffer_[valid_cursor] = values_buffer_[i];
        }
        valid_cursor++;
      }
    }
    buffer_count_ = valid_cursor;

    // 6. Finalize loaded buffer
    if (buffer_count_ > 0) {
      buffer_idx_ = 0;
      valid_ = true;
      return;
    }
  }
}

void SABITableIterator::SeekToFirst() {
  // A failure is sticky: never restart a scan that already lost data.
  if (!status_.ok()) {
    valid_ = false;
    return;
  }

  // The SABI block failed to load in the constructor: nothing to scan.
  if (sabi_reader_ == nullptr) {
    valid_ = false;
    return;
  }
  cur_target_block_idx_ = -1;
  buffer_idx_ = 0;
  buffer_count_ = 0;
  bitmap_iter_ = query_bitmap_->begin();
  LoadNextBlock();
}

void SABITableIterator::Next() {
  assert(valid_);

  // 1. Move buffer cursor to next buffer entry
  buffer_idx_++;

  // 2. If all buffer entries consumed, load next block
  if (buffer_idx_ >= buffer_count_) {
    valid_ = false;
    LoadNextBlock();
  }
}

Slice SABITableIterator::key() const {
  assert(Valid());
  return keys_buffer_[buffer_idx_];
}

Slice SABITableIterator::value() const {
  assert(Valid());
  return values_buffer_[buffer_idx_];
}