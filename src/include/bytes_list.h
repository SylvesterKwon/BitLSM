#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bit_lsm {

// Packed list of byte strings: one arena plus cumulative end offsets, so N
// short strings cost N * 4 bytes of metadata instead of N std::string
// objects. Holds range binning boundaries (in memory and as the v8 policy
// body) and the builder's dense per-row value buffer.
struct BytesList {
  std::string arena;
  std::vector<uint32_t> ends;

  size_t size() const { return ends.size(); }
  bool empty() const { return ends.empty(); }
  std::string_view operator[](size_t i) const {
    const uint32_t start = i == 0 ? 0 : ends[i - 1];
    return std::string_view(arena.data() + start, ends[i] - start);
  }
  std::string_view back() const { return (*this)[size() - 1]; }
  void push_back(std::string_view v) {
    arena.append(v.data(), v.size());
    ends.push_back(static_cast<uint32_t>(arena.size()));
  }
  void clear() {
    arena.clear();
    ends.clear();
  }

  // Elements must be sorted. Index of the first element > v (size() when
  // none) -- the bin-assignment primitive shared by builder and reader, so
  // the two can never disagree about which bin a value lands in.
  uint32_t UpperBound(std::string_view v) const {
    uint32_t lo = 0, hi = static_cast<uint32_t>(size());
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if ((*this)[mid] <= v)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }
  // Index of the first element >= v.
  uint32_t LowerBound(std::string_view v) const {
    uint32_t lo = 0, hi = static_cast<uint32_t>(size());
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if ((*this)[mid] < v)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  // Wire form (host little-endian u32, like every other fixed field in the
  // blob): [count u32][end u32 x count][arena bytes].
  void Serialize(std::string* out) const {
    const uint32_t count = static_cast<uint32_t>(ends.size());
    out->append(reinterpret_cast<const char*>(&count), sizeof(count));
    out->append(reinterpret_cast<const char*>(ends.data()),
                ends.size() * sizeof(uint32_t));
    out->append(arena);
  }
  // Reads one serialized list at p; returns the byte length consumed. The
  // blob already passed RocksDB's block checksum, so no bounds are checked.
  size_t Parse(const char* p) {
    uint32_t count;
    std::memcpy(&count, p, sizeof(count));
    p += sizeof(count);
    ends.resize(count);
    std::memcpy(ends.data(), p, count * sizeof(uint32_t));
    p += count * sizeof(uint32_t);
    const uint32_t bytes = count == 0 ? 0 : ends.back();
    arena.assign(p, bytes);
    return sizeof(count) + count * sizeof(uint32_t) + bytes;
  }
};

}  // namespace bit_lsm
