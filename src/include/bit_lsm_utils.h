#include "bit_lsm_option.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using Attr = std::variant<double, std::string>;

namespace bit_lsm {
// Encode given attritbutes, payload in string
inline void EncodeValue(const BitLSMOptions& options,
                        const std::vector<Attr>& attrs,
                        std::string_view payload, std::string& out_value) {
  uint32_t attr_cnt = attrs.size();
  uint32_t header_size =
      sizeof(uint32_t) + (attr_cnt * sizeof(uint32_t)) + sizeof(uint32_t);
  uint32_t data_size = 0;
  for (uint32_t i = 0; i < attr_cnt; i++) {
    if (options.attr_types[i] == AttrType::CONTINUOUS)
      data_size += sizeof(double);
    else
      data_size += std::get<std::string>(attrs[i]).size();
  }

  out_value.resize(header_size + data_size + payload.size());
  char* base_ptr = out_value.data();

  std::memcpy(base_ptr, &attr_cnt, sizeof(uint32_t));
  uint32_t offset_write_pos = sizeof(uint32_t);
  char* data_ptr = base_ptr + header_size;

  for (uint32_t i = 0; i < attr_cnt; i++) {
    // Put offset
    uint32_t cur_offset = static_cast<uint32_t>(data_ptr - base_ptr);
    std::memcpy(base_ptr + offset_write_pos, &cur_offset, sizeof(uint32_t));
    offset_write_pos += sizeof(uint32_t);

    // Put value
    if (options.attr_types[i] == AttrType::CONTINUOUS) {
      double val = std::get<double>(attrs[i]);
      std::memcpy(data_ptr, &val, sizeof(val));
      data_ptr += sizeof(double);
    } else {
      const std::string& str = std::get<std::string>(attrs[i]);
      std::memcpy(data_ptr, str.data(), str.size());
      data_ptr += str.size();
    }
  }

  // Put payload & offset
  uint32_t payload_offset = static_cast<uint32_t>(data_ptr - base_ptr);
  std::memcpy(base_ptr + offset_write_pos, &payload_offset, sizeof(uint32_t));
  if (!payload.empty())
    std::memcpy(data_ptr, payload.data(), payload.size());
}

using AttrView = std::variant<double, std::string_view>;

// Decode
inline AttrView DecodeAttr(AttrType type, std::string_view buffer,
                           uint32_t attr_idx) {
  const char* base_ptr = buffer.data();
  uint32_t attr_count;
  std::memcpy(&attr_count, base_ptr, sizeof(uint32_t));

  uint32_t offset_pos = sizeof(uint32_t) + (attr_idx * sizeof(uint32_t));
  uint32_t cur_offset;
  std::memcpy(&cur_offset, base_ptr + offset_pos, sizeof(uint32_t));

  const char* data_ptr = base_ptr + cur_offset;
  if (type == AttrType::CONTINUOUS) {
    double val;
    std::memcpy(&val, data_ptr, sizeof(double));
    return val;
  } else {
    // size = (offset[idx+1] - offset[idx])
    // This is safe even for idx = N-1 since we always have payload offset
    uint32_t nxt_offset;
    std::memcpy(&nxt_offset, base_ptr + offset_pos + sizeof(uint32_t),
                sizeof(uint32_t));
    return std::string_view(data_ptr, nxt_offset - cur_offset);
  }
}
} // namespace bit_lsm