#pragma once

#include "rocksdb/slice.h"
#include "util/coding.h"

inline void DecodeIndexValue(rocksdb::Slice& data,
                             std::vector<rocksdb::Slice>* result) {
  uint32_t total_cnt;
  rocksdb::GetFixed32(&data, &total_cnt);

  result->resize(total_cnt);
  for (rocksdb::Slice& ri : *result) {
    GetLengthPrefixedSlice(&data, &ri);
  }
}

inline void EncodeIndexValue(const std::vector<rocksdb::Slice>* src,
                             std::string* dest) {
  rocksdb::PutFixed32(dest, static_cast<uint32_t>(src->size()));

  for (rocksdb::Slice const& i : *src) {
    PutLengthPrefixedSlice(dest, i);
  }
}

inline std::string internal_si_key(uint32_t idx_no, const rocksdb::Slice& key) {
  return std::to_string(idx_no) + ":" + key.ToString();
};