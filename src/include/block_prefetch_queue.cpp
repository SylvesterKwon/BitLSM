#include "block_prefetch_queue.h"

#include <algorithm>
#include <atomic>
#include <iostream>

#include "rocksdb/advanced_cache.h"

namespace bit_lsm {
using namespace rocksdb;

namespace {
std::atomic<uint64_t> g_served{0};
std::atomic<bool> g_async_unavailable{false};
}  // namespace

// Read by RocksDbIOUringEnable below, from any thread that opens a file.
std::atomic<bool> g_rocksdb_io_uring{false};

void EnableRocksDbIOUring() {
  g_rocksdb_io_uring.store(true, std::memory_order_relaxed);
}

BlockPrefetchQueueStats GetBlockPrefetchQueueStats() {
  return {g_served.load(std::memory_order_relaxed),
          g_async_unavailable.load(std::memory_order_relaxed)};
}

void ResetBlockPrefetchQueueStats() {
  g_served.store(0, std::memory_order_relaxed);
  g_async_unavailable.store(false, std::memory_order_relaxed);
}

BlockPrefetchQueue::BlockPrefetchQueue(BlockBasedTable* bbt, uint32_t depth)
    : bbt_(bbt), slots_(depth) {}

void BlockPrefetchQueue::Prepare(const TargetList* targets) {
  targets_ = targets;
  next_submit_ = 0;
  TopUp(0);
}

void BlockPrefetchQueue::TopUp(size_t idx) {
  if (slots_.empty() || targets_ == nullptr) return;
  // Stop at idx + depth: one past it is the slot serving `idx`.
  const size_t limit = std::min(idx + slots_.size(), targets_->size());
  while (next_submit_ < limit) {
    Submit(next_submit_);
    ++next_submit_;
  }
}

void BlockPrefetchQueue::Submit(size_t target_idx) {
  Slot& slot = slots_[target_idx % slots_.size()];
  slot.target = target_idx;
  slot.submitted = false;

  // Once RocksDB has said it cannot read asynchronously it never will, so stop
  // paying for the attempt on every remaining block.
  if (g_async_unavailable.load(std::memory_order_relaxed)) return;

  const BlockHandle& handle = (*targets_)[target_idx].second;
  // Prefetching a cached block is pure extra device I/O.
  if (InBlockCache(handle)) return;

  const BlockBasedTable::Rep* rep = bbt_->get_rep();
  if (slot.buf == nullptr) {
    // No readahead: this block's bytes and nothing else.
    ReadaheadParams params;
    params.num_buffers = 1;
    rep->CreateFilePrefetchBuffer(params, &slot.buf,
                                  /*readaheadsize_cb=*/nullptr,
                                  FilePrefetchBufferUsage::kUserScanPrefetch);
  }

  IOOptions opts;
  IODebugContext dbg;
  const ReadOptions read_options;
  if (!rep->file->PrepareIOOptions(read_options, opts, &dbg).ok()) return;

  Slice result;
  // TryAgain = submitted and in flight; OK = the buffer already held it.
  const Status s = slot.buf->PrefetchAsync(
      opts, rep->file.get(), handle.offset(),
      BlockBasedTable::BlockSizeWithTrailer(handle), &result);
  slot.submitted = s.ok() || s.IsTryAgain();
  if (!slot.submitted && s.IsNotSupported() &&
      !g_async_unavailable.exchange(true, std::memory_order_relaxed)) {
    std::cerr
        << "[bitlsm] scan_prefetch_depth is set but this build cannot "
           "issue async reads (no liburing); the option will do nothing\n";
  }
}

bool BlockPrefetchQueue::InBlockCache(const BlockHandle& handle) const {
  const BlockBasedTable::Rep* rep = bbt_->get_rep();
  Cache* cache = rep->table_options.block_cache.get();
  if (cache == nullptr) return false;
  const CacheKey key =
      BlockBasedTable::GetCacheKey(rep->base_cache_key, handle);
  Cache::Handle* h = cache->BasicLookup(key.AsSlice(), /*stats=*/nullptr);
  if (h == nullptr) return false;
  cache->Release(h);
  return true;
}

FilePrefetchBuffer* BlockPrefetchQueue::BufferFor(size_t idx) {
  if (slots_.empty() || targets_ == nullptr || idx >= targets_->size())
    return nullptr;
  TopUp(idx);
  Slot& slot = slots_[idx % slots_.size()];
  // A different target means this block was skipped or revisited out of order.
  if (slot.target != idx || !slot.submitted) return nullptr;
  g_served.fetch_add(1, std::memory_order_relaxed);
  return slot.buf.get();
}

}  // namespace bit_lsm

// RocksDB's weak io_uring opt-in. Defining it here means anything linking
// BitLSM cannot also define it; RocksDB's tests do, but WITH_TESTS is off.
extern "C" bool RocksDbIOUringEnable() {
  return bit_lsm::g_rocksdb_io_uring.load(std::memory_order_relaxed);
}
