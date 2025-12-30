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

inline void EncodeIndexValue(const std::vector<rocksdb::Slice>* input,
                             std::string* dest) {
  rocksdb::PutFixed32(dest, static_cast<uint32_t>(input->size()));

  for (rocksdb::Slice const& i : *input) {
    PutLengthPrefixedSlice(dest, i);
  }
}