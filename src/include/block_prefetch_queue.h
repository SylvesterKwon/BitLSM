#pragma once
// Keeps `depth` of a scan's data-block reads in flight. RocksDB's implicit
// readahead only ramps while reads stay adjacent, and a bitmap picks blocks
// with gaps between them.
//
// One FilePrefetchBuffer per in-flight read, because an async prefetch is
// served only to a read at exactly its offset. Blocks are consumed through the
// ordinary BlockFetcher path, so checksum, decompression and block-cache
// insertion are unchanged.
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "file/file_prefetch_buffer.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"

namespace bit_lsm {

// Process-wide; these reads bypass BlockBasedTable, so RocksDB's tickers do
// not see them.
struct BlockPrefetchQueueStats {
  uint64_t served = 0;  // blocks read out of a queue buffer
  // Set the first time RocksDB refuses an async read: the build has no
  // liburing.
  bool async_unavailable = false;
};
BlockPrefetchQueueStats GetBlockPrefetchQueueStats();
void ResetBlockPrefetchQueueStats();

// True once RocksDB has refused an async read in this process; every async
// submitter checks it before trying.
bool AsyncReadUnavailable();
// Records the refusal; the first call prints a note. Reset only by
// ResetBlockPrefetchQueueStats.
void NoteAsyncReadUnavailable();

// RocksDB gates io_uring behind the weak RocksDbIOUringEnable symbol; without
// it ReadAsync answers NotSupported. Call before opening a DB, since the
// choice is made per file open. Process-wide and one-way: a second DB opened
// without the prefetch queue must not take the rings away from the first.
void EnableRocksDbIOUring();

class BlockPrefetchQueue {
 public:
  // Target blocks in file order, as SABITableIterator builds them:
  // {block index, handle}.
  using TargetList = std::vector<std::pair<uint32_t, rocksdb::BlockHandle>>;

  // depth 0 disables the queue; `bbt` must outlive this object.
  BlockPrefetchQueue(rocksdb::BlockBasedTable* bbt, uint32_t depth);

  // Binds to a target list, which must outlive this queue, and submits the
  // first `depth` reads.
  void Prepare(const TargetList* targets);

  // Buffer holding target `idx`, or nullptr when it was not prefetched; the
  // caller then reads the block the ordinary way.
  rocksdb::FilePrefetchBuffer* BufferFor(size_t idx);

 private:
  static constexpr size_t kNoTarget = std::numeric_limits<size_t>::max();

  // Slot i serves targets i, i + depth, ... Reuse is safe: BlockFetcher
  // copies a prefetched block onto the heap.
  struct Slot {
    std::unique_ptr<rocksdb::FilePrefetchBuffer> buf;
    size_t target = kNoTarget;
    bool submitted = false;
  };

  void TopUp(size_t idx);  // fills the window ahead of idx
  void Submit(size_t target_idx);
  bool InBlockCache(const rocksdb::BlockHandle& handle) const;

  rocksdb::BlockBasedTable* bbt_;
  const TargetList* targets_ = nullptr;
  std::vector<Slot> slots_;
  size_t next_submit_ = 0;
};

}  // namespace bit_lsm
