#pragma once
#include "rocksdb/slice.h"
#include "util/coding.h"
#include <cstring>
#include <iostream>
#include <queue>
#include <string>

// PK-list slice reader
class IndexStreamReader {
public:
  IndexStreamReader(const rocksdb::Slice* slice) : cnt_(0), valid_(false) {
    if (slice && slice->size() >= 4) {
      data_ = *slice;
      if (!rocksdb::GetFixed32(&data_, &cnt_)) {
        cnt_ = 0;
      }
      Next();
    }
  }
  bool Valid() const { return valid_; }
  rocksdb::Slice Current() const { return current_; }
  void Next() {
    if (cnt_ > 0) {
      if (rocksdb::GetLengthPrefixedSlice(&data_, &current_)) {
        cnt_--;
        valid_ = true;
      } else {
        valid_ = false;
      }
    } else {
      valid_ = false;
    }
  }
  rocksdb::Slice RestRawData() const { return data_; }
  uint32_t RemainingCount() const { return (valid_ ? 1 : 0) + cnt_; }

private:
  rocksdb::Slice data_;    // Left datastream
  rocksdb::Slice current_; // Current item
  uint32_t cnt_;           // # of left item in data_
  bool valid_;
};

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

// Merge multiple index value
inline void MergeIndexValue(const std::vector<rocksdb::Slice>& operand_list,
                            std::string* dest) {
  uint32_t size_sum = 0;
  for (const auto& oi : operand_list)
    size_sum += oi.size();
  dest->reserve(size_sum);

  std::vector<IndexStreamReader> iter_list;
  iter_list.reserve(operand_list.size());
  for (const auto& operand : operand_list) {
    iter_list.emplace_back(&operand);
  }

  // PQ node definition
  struct HeapNode {
    rocksdb::Slice val; // Slice value
    uint32_t idx;       // Index of iterator list
    bool operator>(const HeapNode& other) const {
      return val.compare(other.val) > 0;
    }
  };

  std::priority_queue<HeapNode, std::vector<HeapNode>, std::greater<HeapNode>>
      pq;

  // Push first value from each iterator
  for (uint32_t i = 0; i < iter_list.size(); ++i) {
    if (iter_list[i].Valid())
      pq.push({iter_list[i].Current(), i});
  }

  rocksdb::PutFixed32(dest, 0); // This will be replaced as actual cnt
  uint32_t cnt = 0;

  rocksdb::Slice last_val; // to remove duplicates
  bool is_first = true;

  // K-Way Merge Loop
  while (!pq.empty()) {
    HeapNode top = pq.top();
    pq.pop();

    if (is_first || top.val.compare(last_val) != 0) {
      rocksdb::PutLengthPrefixedSlice(dest, top.val);
      last_val = top.val;
      cnt++;
      is_first = false;
    }

    auto& it = iter_list[top.idx];
    it.Next();
    if (it.Valid()) {
      pq.push({it.Current(), top.idx});
    }
  }

  rocksdb::EncodeFixed32(reinterpret_cast<char*>(dest->data()), cnt);
}

// Merge two index value
inline void MergeIndexValue(const rocksdb::Slice* a, const rocksdb::Slice* b,
                            std::string* dest) {
  dest->reserve(a->size() + b->size());
  IndexStreamReader iterA(a), iterB(b);
  rocksdb::PutFixed32(dest, 0);
  uint32_t cnt = 0;

  while (iterA.Valid() && iterB.Valid()) {
    int cmp = iterA.Current().compare(iterB.Current());
    if (cmp < 0) {
      rocksdb::PutLengthPrefixedSlice(dest, iterA.Current());
      iterA.Next();
    } else if (cmp > 0) {
      rocksdb::PutLengthPrefixedSlice(dest, iterB.Current());
      iterB.Next();
    } else {
      rocksdb::PutLengthPrefixedSlice(dest, iterA.Current());
      iterA.Next(), iterB.Next();
    }
    ++cnt;
  }
  if (iterA.Valid()) {
    // Put current value
    rocksdb::PutLengthPrefixedSlice(dest, iterA.Current());
    cnt++;
    // Put remaining raw value
    cnt += iterA.RemainingCount() - 1;
    rocksdb::Slice rest = iterA.RestRawData();
    dest->append(rest.data(), rest.size());
  } else if (iterB.Valid()) {
    rocksdb::PutLengthPrefixedSlice(dest, iterB.Current());
    cnt++;
    cnt += iterB.RemainingCount() - 1;
    rocksdb::Slice rest = iterB.RestRawData();
    dest->append(rest.data(), rest.size());
  }

  rocksdb::EncodeFixed32(reinterpret_cast<char*>(dest->data()), cnt);
}

// ---------- Order-preserving double encoding ----------
// IEEE 754 bit trick: flip sign bit for positives, flip all bits for negatives.
// Result: memcmp on 8-byte encoded form == numeric < on doubles.

inline void EncodeDoubleOrderPreserving(double val, char* out) {
  uint64_t bits;
  std::memcpy(&bits, &val, sizeof(bits));
  if (bits & (uint64_t{1} << 63))
    bits = ~bits;                    // negative: flip all
  else
    bits ^= (uint64_t{1} << 63);    // positive: flip sign bit
  // big-endian
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<char>(bits & 0xFF);
    bits >>= 8;
  }
}

inline double DecodeDoubleOrderPreserving(const char* data) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i)
    bits = (bits << 8) | static_cast<uint8_t>(data[i]);
  if (bits & (uint64_t{1} << 63))
    bits ^= (uint64_t{1} << 63);    // was positive: flip sign bit back
  else
    bits = ~bits;                    // was negative: flip all back
  double val;
  std::memcpy(&val, &bits, sizeof(val));
  return val;
}

inline std::string EncodeDoubleForSIKey(double val) {
  std::string s(8, '\0');
  EncodeDoubleOrderPreserving(val, s.data());
  return s;
}

// Encode into a reusable buffer (hot-path friendly)
inline void EncodeDoubleForSIKey(double val, std::string& out) {
  out.resize(8);
  EncodeDoubleOrderPreserving(val, out.data());
}

// Return index SI key (string secondary key)
inline std::string GetInternalSIKey(uint32_t idx_no, const rocksdb::Slice& key,
                                    uint32_t idx_no_prefix_size = 4) {
  std::string res = std::to_string(idx_no);
  assert(res.size() <= idx_no_prefix_size);
  res.resize(idx_no_prefix_size, ' '); // Add padding to make it fix-sized
  res += key.ToStringView();
  return res;
}

// Return index SI key (double secondary key, order-preserving)
inline std::string GetInternalSIKey(uint32_t idx_no, double sk_value,
                                    uint32_t idx_no_prefix_size = 4) {
  std::string res = std::to_string(idx_no);
  assert(res.size() <= idx_no_prefix_size);
  res.resize(idx_no_prefix_size, ' ');
  char buf[8];
  EncodeDoubleOrderPreserving(sk_value, buf);
  res.append(buf, 8);
  return res;
}

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

  if (current == index)
    return str.substr(start);

  return {};
}
