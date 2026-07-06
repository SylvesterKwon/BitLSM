#include <bit_lsm_iterator.h>
#include <bit_lsm_query.h>

#include <cstdint>
#include <iostream>

#include "db/version_set.h"
#include "rocksdb/options.h"
#include "sabi.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;
using namespace roaring;

void BitLSMMemTableIterator::FindNextValidEntry() {
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

    // 4. Filter query condition
    if (compiled_.Eval(iter_->value())) {
      valid_ = true;
      return;
    }

    iter_->Next();
  }
}

BitLSMMemTableIterator::BitLSMMemTableIterator(rocksdb::MemTable* mem,
                                               const BitLSMOptions& options,
                                               const CompiledQuery& compiled)
    : options_(options), mem_(mem), compiled_(compiled), iter_(nullptr) {
  assert(mem_ != nullptr);
  ReadOptions ro;
  iter_ = mem_->NewIterator(ro, nullptr, &arena_, nullptr, false);
}

BitLSMMemTableIterator::~BitLSMMemTableIterator() {
  // Since iter_'s memory space is managed by arena, use destruct instead of
  // delete
  if (iter_ != nullptr) iter_->~InternalIterator();
}

void BitLSMMemTableIterator::SeekToFirst() {
  iter_->SeekToFirst();
  FindNextValidEntry();
}

void BitLSMMemTableIterator::Next() {
  assert(Valid());
  iter_->Next();
  FindNextValidEntry();
}

Slice BitLSMMemTableIterator::key() const {
  assert(Valid());
  return iter_->key();
}

Slice BitLSMMemTableIterator::value() const {
  assert(Valid());
  return iter_->value();
}
