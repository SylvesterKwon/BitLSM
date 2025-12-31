#pragma once

#include "rocksdb/slice.h"
#include "util/coding.h"
#include <sstream>
#include <string>

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

// Return index SI key
inline std::string GetInternalSIKey(uint32_t idx_no, const rocksdb::Slice& key,
                                    uint32_t idx_no_prefix_size = 4) {
  std::string res = std::to_string(idx_no);
  assert(res.size() <= idx_no_prefix_size);
  res.resize(idx_no_prefix_size, ' '); // Add padding to make it fix-sized
  res += key.ToStringView();
  return res;
};

// Get i-th token of given string and delimiter. Returns empty string if there's
// no i-th token
// Warning - don't use this in iteration, causes O(N^2) complexity
// since it uses while-getline
inline std::string GetIthToken(const std::string& str, int index,
                               char delim = ',') {
  std::stringstream ss(str);
  std::string token;
  int current = 0;

  while (getline(ss, token, delim)) {
    if (current == index)
      return token;
    current++;
  }
  return "";
}