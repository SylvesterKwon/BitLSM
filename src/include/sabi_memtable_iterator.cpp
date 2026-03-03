#include "db/version_set.h"
#include "rocksdb/options.h"
#include "sabi.h"
#include <cstdint>
#include <iostream>
#include <sabi_iterator.h>
#include <sabi_query.h>

using namespace std;
using namespace rocksdb;
using namespace bitmap_index;
using namespace roaring;

void SABIMemTableIterator::FindNextValidEntry() {
  valid_ = false;
  Status s;

  while (iter_->Valid()) {
    ParsedInternalKey ikey;

    // 1. Skip corrupted key
    s = rocksdb::ParseInternalKey(iter_->key(), &ikey, false);
    if (!s.ok()) {
      iter_->Next();
      continue;
    }

    // 2. MVCC filtering
    if (ikey.sequence > options_.read_seqno) {
      iter_->Next();
      continue;
    }

    // 3. Filter tombstone
    if (ikey.type == rocksdb::kTypeDeletion ||
        ikey.type == rocksdb::kTypeSingleDeletion) {
      iter_->Next();
      continue;
    }

    // 4 Filter query condition
    if (query_.CheckCondition(iter_->value(), options_)) {
      valid_ = true;
      return;
    }

    iter_->Next();
  }
}

SABIMemTableIterator::SABIMemTableIterator(rocksdb::MemTable* mem,
                                           SABIOptions options, SABIQuery query)
    : options_(options), mem_(mem), query_(std::move(query)), iter_(nullptr) {
  assert(mem_ != nullptr);
  ReadOptions ro;
  iter_ = mem_->NewIterator(ro, nullptr, &arena_, nullptr, false);
}

SABIMemTableIterator::~SABIMemTableIterator() {
  // Since iter_'s memory space is managed by arena, use destruct instead of
  // delete
  if (iter_ != nullptr)
    iter_->~InternalIterator();
}

void SABIMemTableIterator::SeekToFirst() {
  iter_->SeekToFirst();
  FindNextValidEntry();
}

void SABIMemTableIterator::Next() {
  assert(Valid());
  iter_->Next();
  FindNextValidEntry();
}

Slice SABIMemTableIterator::key() const {
  assert(Valid());
  return iter_->key();
}

Slice SABIMemTableIterator::value() const {
  assert(Valid());
  return iter_->value();
}

void SABIMemTableIterator::TEST_DumpValue(Slice input) {
  for (uint32_t i = 0; i < options_.sk_num; ++i) {
    if (i)
      cout << " / ";
    rocksdb::Slice part;
    GetLengthPrefixedSlice(&input, &part);
    if (options_.sk_types[i] == SKType::CATEGORICAL) {
      cout << part.ToString();
    } else if (options_.sk_types[i] == SKType::CONTINUOUS) {
      string s = part.ToString();
      cout << s;
    }
  }
  cout << "\n";
}
