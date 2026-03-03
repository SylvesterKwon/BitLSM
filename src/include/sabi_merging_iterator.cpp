#include "db/version_set.h"
#include "rocksdb/options.h"
#include "sabi.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/format.h"
#include <iostream>
#include <sabi_iterator.h>
#include <sabi_query.h>

using namespace std;
using namespace rocksdb;
using namespace bitmap_index;

SABIMergingIterator::SABIMergingIterator(ColumnFamilyData* cfd,
                                         SABIOptions options, SABIQuery query)
    : cfd_(cfd), options_(options), query_(query), v_(cfd->current()),
      tc_(cfd->table_cache()), storage_info_(v_->storage_info()),
      icmp_(storage_info_->InternalComparator()),
      cf_opts_(cfd->GetLatestMutableCFOptions()),
      heap_(IteratorComparator(icmp_)) {
  // 1. Protect current version
  v_->Ref();

  // TODO: 위 맴버 변수중에 level iterator로 위임한것들 있어서 뺄 수 있는건
  // 빼야함

  // 2. Add MemTable iterators
  // TODO: implement this

  // 3. Add L0 iterators
  TableCache::CacheInterface cache_interface = tc_->get_cache();
  const vector<FileMetaData*>& l0_files = storage_info_->LevelFiles(0);
  for (FileMetaData* meta : l0_files) {
    TableCache::TypedHandle* table_handle = nullptr;
    ReadOptions ro;
    Status s = tc_->FindTable(ro, FileOptions(), *icmp_, *meta, &table_handle,
                              cf_opts_);
    if (!s.ok()) {
      std::cerr << "[SABIMergingIterator]: Failed to load L0 file "
                << meta->fd.GetNumber() << "\n";
      continue;
    }
    TableReader* table = cache_interface.Value(table_handle);
    BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);

    // cout << "[SABIMergingIterator] Added L0 SST iterator\n";
    ch_iters_.push_back(new SABITableIterator(options_, bbt, query_));
    l0_handles_.push_back(table_handle);
  }

  // 4. Add L1+ level iterators
  for (uint32_t level = 1; level < storage_info_->num_non_empty_levels();
       ++level) {
    if (storage_info_->NumLevelFiles(level) > 0) {
      ch_iters_.push_back(new SABILevelIterator(cfd_, level, options_, query_));
    }
  }
};

SABIMergingIterator::~SABIMergingIterator() {
  // 1. Free children iterators
  for (auto* ch : ch_iters_)
    delete ch;
  ch_iters_.clear();

  // 2. Release cache for L0 SST
  for (auto* handle : l0_handles_)
    tc_->get_cache().Release(handle);
  l0_handles_.clear();

  // 3. Free current version
  if (v_)
    v_->Unref();
}

void SABIMergingIterator::SeekToFirst() {
  // 1. Clear existing heap
  while (!heap_.empty())
    heap_.pop();
  valid_ = false;

  // 2. Propagate SeekToFirst & Register to heap
  for (auto* child : ch_iters_) {
    child->SeekToFirst();
    if (child->Valid())
      heap_.push(child);
  }

  // 3. Set current iterator
  if (!heap_.empty()) {
    valid_ = true;
  }
}

void SABIMergingIterator::Next() {
  assert(Valid());
  // cout << "[SABIMergingIterator] Next() Called\n";

  // 1. Forward top iterator in heap
  SABIInternalIterator* top_iter = heap_.top();
  heap_.pop();
  top_iter->Next();
  if (top_iter->Valid())
    heap_.push(top_iter);

  // 2. Update valid_
  valid_ = !heap_.empty();
}

Slice SABIMergingIterator::key() const {
  assert(Valid());
  return heap_.top()->key();
}

Slice SABIMergingIterator::value() const {
  assert(Valid());
  return heap_.top()->value();
}

void SABIMergingIterator::TEST_DumpValue(Slice input) {
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