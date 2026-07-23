#include <bit_lsm_iterator.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "bit_lsm_option.h"
#include "bit_lsm_utils.h"
#include "rocksdb/db.h"
#include "rocksdb/snapshot.h"

using namespace std;
using namespace rocksdb;
using namespace bit_lsm;

namespace {
// Validates constructor arguments from the first member initializer, before
// any member acquires a resource, so a rejected construction leaks nothing.
DB* ValidateIteratorArgs(DB* db, ResultMode result_mode,
                         const Snapshot* snapshot) {
  if (result_mode == ResultMode::Candidate && snapshot == nullptr)
    throw std::invalid_argument(
        "Candidate mode requires an injected snapshot: candidate generation "
        "and the consumer's fetch must see the same seqno");
  return db;
}
}  // namespace

BitLSMIterator::BitLSMIterator(DB* db, ColumnFamilyHandle* cfh,
                               const BitLSMOptions& options,
                               const BitLSMQuery& query, ResultMode result_mode,
                               const Snapshot* snapshot)
    : db_(ValidateIteratorArgs(db, result_mode, snapshot)),
      db_impl_(static_cast<DBImpl*>(db_)),
      cfh_(cfh),
      // 1. Use the injected snapshot if given (caller keeps ownership),
      // otherwise create and own one
      snapshot_(snapshot != nullptr ? snapshot : db_->GetSnapshot()),
      snapshot_owned_(snapshot == nullptr),
      // Scan the CF the handle refers to; null falls back to the default CF
      cfd_(cfh != nullptr
               ? static_cast<ColumnFamilyHandleImpl*>(cfh)->cfd()
               : db_impl_->GetVersionSet()->GetColumnFamilySet()->GetDefault()),
      // 2. Create SuperVersion
      sv_(cfd_->GetReferencedSuperVersion(db_impl_)),
      options_(options),
      query_(query),
      result_mode_(result_mode),
      // Candidate mode skips per-row verification: leave compiled_ an inert
      // empty shell and signal via a null query_ctx_.compiled
      compiled_(result_mode == ResultMode::Candidate
                    ? CompiledQuery()
                    : CompiledQuery(query, options)),
      sabi_query_(EncodeQuery(query_, options)),
      query_ctx_{options_, sabi_query_,
                 result_mode == ResultMode::Candidate ? nullptr : &compiled_},
      scan_ctx_(sv_),
      latest_user_key_added("") {
  // 3. Save snapshot's seqno to SABIOption
  options_.read_seqno = snapshot_->GetSequenceNumber();

  // 4. Create merging iterator
  smi_ = new BitLSMMergingIterator(scan_ctx_, query_ctx_);
}

BitLSMIterator::~BitLSMIterator() {
  // 1. Free BitLSMMergingIterator
  delete smi_;

  // 2. Clean up super version
  if (sv_->Unref()) {
    db_impl_->mutex()->Lock();
    sv_->Cleanup();
    db_impl_->mutex()->Unlock();
    delete sv_;
  }

  // 3. Release snapshot (only if this iterator created it; injected
  // snapshots are owned by the caller)
  if (snapshot_owned_ && snapshot_ != nullptr) db_->ReleaseSnapshot(snapshot_);
}

void BitLSMIterator::FetchNextBatch(uint32_t batch_size) {
  Status s;

  // 1. Clean current batch
  batch_keys_.clear();
  batch_values_.clear();
  batch_cur_idx_ = 0;
  valid_ = false;

  // 2. Try to find next valid batch which contains at least one valid data
  // entry
  while (batch_keys_.empty() && smi_->Valid()) {
    candidate_keys_.clear();
    candidate_keys_.reserve(batch_size);

    // 3. Get candidate keys
    while (smi_->Valid() && candidate_keys_.size() < batch_size) {
      ParsedInternalKey ikey;
      s = rocksdb::ParseInternalKey(smi_->key(), &ikey, false);
      if (s.ok()) {
        // Compared as a Slice, so a duplicate costs a memcmp, not a copy.
        if (ikey.user_key != Slice(latest_user_key_added)) {
          candidate_keys_.emplace_back(ikey.user_key.data(),
                                       ikey.user_key.size());
          latest_user_key_added.assign(ikey.user_key.data(),
                                       ikey.user_key.size());
        }
      }
      smi_->Next();
    }
    if (candidate_keys_.empty()) break;

    // Candidate mode: the bitmap-pruned keys are the result. Skip the
    // authoritative fetch and per-row verification; the consumer fetches
    // with its own snapshot and re-verifies.
    if (result_mode_ == ResultMode::Candidate) {
      // swap, not move-assign: a move would leave candidate_keys_ with no
      // buffer for the next batch.
      batch_keys_.swap(candidate_keys_);
      continue;
    }

    // 4. MultiGet candidates from RocksDB
    vector<Slice> candidate_key_slices;
    candidate_key_slices.reserve(candidate_keys_.size());
    for (const auto& k : candidate_keys_) {
      candidate_key_slices.push_back(Slice(k));
    }
    vector<PinnableSlice> pin_values(candidate_keys_.size());
    vector<Status> statuses(candidate_keys_.size());
    // vector<string> db_values;
    ReadOptions ro;
    ro.snapshot = snapshot_;

    db_->MultiGet(ro, cfh_, candidate_key_slices.size(),
                  candidate_key_slices.data(), pin_values.data(),
                  statuses.data(), true);

    // 5. Cross check
    for (uint32_t i = 0; i < candidate_key_slices.size(); ++i) {
      // 5-1. Check given candidate key exists in DB
      if (!statuses[i].ok()) continue;

      // 5-2. Value validation (nullptr compiled = consumer re-verifies)
      if (!query_ctx_.compiled || query_ctx_.compiled->Eval(pin_values[i])) {
        // 5-3. Move key/value to validated batch if valid entry
        batch_keys_.push_back(std::move(candidate_keys_[i]));
        // Optimization: Only call ToString() to valid value
        batch_values_.push_back(pin_values[i].ToString());
      }
    }
  }

  // 6. Update valid_
  if (!batch_keys_.empty()) valid_ = true;
}

void BitLSMIterator::SeekToFirst() {
  // 1. SeekToFirst internal iterator
  smi_->SeekToFirst();
  latest_user_key_added.clear();

  // 2. Prepare next batch
  FetchNextBatch(1024);  // TODO: parameterize it.
}

void BitLSMIterator::Next() {
  assert(Valid());

  // 1. Forward batch cursor
  batch_cur_idx_++;

  // 2. If all entry in batch consumed, Prepare next batch
  if (batch_cur_idx_ >= batch_keys_.size()) {
    FetchNextBatch(1024);
  }
}

Slice BitLSMIterator::key() const {
  assert(Valid());
  return batch_keys_[batch_cur_idx_];
}

Slice BitLSMIterator::value() const {
  assert(Valid());
  // No authoritative fetch happens in Candidate mode, so there is no value
  // to return; a thrown error (not an assert, which Release compiles out)
  // keeps a misusing consumer from reading garbage.
  if (result_mode_ == ResultMode::Candidate)
    throw std::logic_error(
        "BitLSMIterator::value() is unavailable in Candidate mode; fetch "
        "authoritatively by key() instead");
  return batch_values_[batch_cur_idx_];
}