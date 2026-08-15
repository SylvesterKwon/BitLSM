#pragma once
// Submits `depth` of a scan's data-block reads at once instead of one at a
// time. RocksDB's implicit readahead cannot do this: its ramp only grows while
// reads stay adjacent (BlockPrefetcher::PrefetchIfNeeded -> IsBlockSequential)
// and a bitmap picks blocks with gaps between them, so it resets on nearly
// every read.
//
// One FilePrefetchBuffer per in-flight read, because an explicitly submitted
// async prefetch is served only to the consumer whose offset equals the
// prefetched one -- TryReadFromCacheUntracked aborts the buffer and falls back
// for any other offset. Blocks are consumed through the ordinary BlockFetcher
// path, so checksum, decompression and block-cache insertion are unchanged.
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
// not see them. This feature fails silently -- wrong wiring and a build with
// no io_uring both leave results correct and speed unchanged -- so it reports
// enough to tell "ran" from "did nothing", and which kind of nothing.
struct BlockPrefetchQueueStats {
  uint64_t served = 0;  // blocks read out of a queue buffer
  // Set the first time RocksDB refuses an async read, after which the queue
  // stops submitting. Means the build found no liburing.
  bool async_unavailable = false;
};
BlockPrefetchQueueStats GetBlockPrefetchQueueStats();
void ResetBlockPrefetchQueueStats();

// RocksDB hides io_uring behind a weak RocksDbIOUringEnable symbol the
// application must define (env/fs_posix.cc:78); undefined, every
// PosixRandomAccessFile gets null ring pointers and ReadAsync answers
// NotSupported. Call before opening a DB -- the choice is made per file open.
void EnableRocksDbIOUring(bool enable);

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

  // Buffer holding target `idx`, or nullptr if it was not prefetched (off,
  // already cached, or submission failed). Null is not an error: the caller
  // reads the block the ordinary way.
  rocksdb::FilePrefetchBuffer* BufferFor(size_t idx);

 private:
  static constexpr size_t kNoTarget = std::numeric_limits<size_t>::max();

  // Slot i serves targets i, i + depth, ... Reuse is safe: BlockFetcher copies
  // a prefetched block onto the heap (CopyBufferToHeapBuf), so nothing aliases
  // the buffer afterwards.
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
