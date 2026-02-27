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

SABIMergingIterator::SABIMergingIterator(ColumnFamilyData* cfd,
                                         SABIOptions options, SABIQuery query)
    : cfd_(cfd), options_(options), query_(query), v_(cfd->current()),
      tc_(cfd->table_cache()), storage_info_(v_->storage_info()),
      icmp_(storage_info_->InternalComparator()),
      cf_opts_(cfd->GetLatestMutableCFOptions()) {
  v_->Ref();
  // WIP - 위 맴버 변수중에 level iterator로 위임한것들 있어서 뺄 수 있는건
  // 빼야함

  const FileMetaData* file_meta = nullptr;
  const bool no_io = false;
  const ReadOptions& read_options = ReadOptions();
  const FileOptions& file_options = FileOptions();

  uint32_t num_levels = storage_info_->num_levels(); // TODO: usethis
  const vector<FileMetaData*>& files = storage_info_->LevelFiles(6);
  assert(!files.empty());
  file_meta = files[0]; // TODO: SST파일 추출 예시
  // WIP - 레벨 이터레이터 짜고 다시 돌아오기
  // version vs superversion 질문하기.
  // version 쓸때 ref , unref 이런거 컨셉한번보기

  TableCache::TypedHandle* table_handle = nullptr;

  Status s = tc_->FindTable(
      read_options, file_options, *icmp_, *file_meta, &table_handle, cf_opts_,
      no_io); // TODO: 뒤에 optional parameter도 의미 파악 필요
  if (!s.ok())
    cout << "Debug: fail to find TableReader\n";

  TableCache::CacheInterface cache_interface = tc_->get_cache();
  TableReader* table = cache_interface.Value(table_handle);
  BlockBasedTable* bbt = static_cast<BlockBasedTable*>(table);
};

SABIMergingIterator::~SABIMergingIterator() {
  if (v_)
    v_->Unref();
}

void SABIMergingIterator::SeekToFirst() {
  // // 1. 힙 초기화 (기존에 들어있던 게 있다면 비움)
  // while (!heap_.empty()) {
  //   heap_.pop();
  // }
  // current_ = nullptr;

  // // 2. 모든 자식 이터레이터 초기화
  // for (auto* child : children_) {
  //   child->SeekToFirst();

  //   // 3. 유효한 데이터가 있는 자식만 힙에 등록
  //   // (데이터가 없거나 쿼리 조건에 맞는 게 하나도 없는 파일은 여기서 탈락함)
  //   if (child->Valid()) {
  //     heap_.push(child);
  //   }
  // }

  // // 4. 힙의 최상단(가장 작은 Key)을 현재 커서로 설정
  // if (!heap_.empty()) {
  //   current_ = heap_.top();
  // }
}

void SABIMergingIterator::Next() {
  // TODO: implement this
  assert(false);
};