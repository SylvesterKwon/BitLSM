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
inline std::string_view GetIthToken(std::string_view str, int index,
                                    char delim = ',') {
  size_t start = 0;
  size_t end = str.find(delim);
  int current = 0;

  while (end != std::string_view::npos) {
    if (current == index) {
      return str.substr(start, end - start);
    }
    start = end + 1;
    end = str.find(delim, start);
    current++;
  }

  // 마지막 토큰 처리 (구분자 뒤에 남은 문자열)
  if (current == index)
    return str.substr(start);

  // 인덱스가 범위 밖이면 빈 view 반환
  return {};
}