#include "db/version_set.h"
#include "rocksdb/options.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"
#include <cstdint>
#include <iostream>
#include <sabi_iterator.h>
#include <sabi_query.h>

using namespace std;
using namespace rocksdb;
using namespace bitmap_index;
using namespace roaring;

SABILevelIterator::SABILevelIterator(ColumnFamilyData* cfd, uint32_t level,
                                     SABIOptions options, SABIQuery query)
    : cfd_(cfd), level_(level), options_(options), query_(std::move(query)),
      v_(cfd->current()), tc_(cfd->table_cache()),
      storage_info_(v_->storage_info()),
      icmp_(storage_info_->InternalComparator()),
      cf_opts_(cfd->GetLatestMutableCFOptions()),
      files_(storage_info_->LevelFiles(level_)), cur_file_idx_(0),
      cur_table_handle_(nullptr), cur_sti_(nullptr) {}

SABILevelIterator::~SABILevelIterator() {
  if (cur_sti_)
    delete cur_sti_;
  if (cur_table_handle_)
    tc_->get_cache().Release(cur_table_handle_);
}

void SABILevelIterator::LoadFile(size_t idx) {
  // cout << "[SABILevelIterator] level " << level_ << ", " << idx
  //      << " th file is loading\n";

  // 1. Clean up existing iterator & table handle
  valid_ = false;
  TableCache::CacheInterface cache_interface = tc_->get_cache();
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

  Status s = tc_->FindTable(read_options, file_options, *icmp_, *file_meta,
                            &new_table_handle, cf_opts_, no_io);
  if (!s.ok()) {
    std::cerr << "Failed to load SST\n";
    return;
  }
  TableReader* table = cache_interface.Value(new_table_handle);
  BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);

  // 4. Prepare new SABITableIterator
  cur_table_handle_ = new_table_handle;
  // TODO: MVCC 를 위한 seqno 전달
  cur_sti_ = new SABITableIterator(options_, bbt, query_);
}

void SABILevelIterator::SeekToFirst() {
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

void SABILevelIterator::Next() {
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

Slice SABILevelIterator::key() const {
  assert(Valid());
  return cur_sti_->key();
}

Slice SABILevelIterator::value() const {
  assert(Valid());
  return cur_sti_->value();
}

void SABILevelIterator::TEST_DumpValue(Slice input) {
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    if (i)
      cout << " / ";
    rocksdb::Slice part;
    GetLengthPrefixedSlice(&input, &part);
    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      cout << part.ToString();
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      // Builder에서 문자열 형태로 저장했으므로 문자열로 출력하되, double 해석
      // 가능 여부 확인
      string s = part.ToString();
      cout << s;
    }
  }
  cout << "\n";
}
