#include "rocksdb/slice.h"
#include "sabi.h"
#include <bit_lsm_query.h>
#include <charconv>

using namespace std;
using namespace rocksdb;

namespace bit_lsm {
bool BitLSMQuery::CheckCondition(rocksdb::Slice slice,
                                 const BitLSMOptions& options) {
  if (conditions.empty())
    return true;

  uint32_t cur_attr_idx = 0;
  uint32_t cur_cond_idx = 0;

  // For all query condition
  while (cur_cond_idx < conditions.size()) {
    uint32_t target_attr_idx = conditions[cur_cond_idx].attr_idx;

    // 1. Skip unnecessary attr
    while (cur_attr_idx < target_attr_idx) {
      // If there's no attr left (maybe new kind of attr is queried) return
      // false
      if (slice.empty())
        return false;
      Slice ignored;
      GetLengthPrefixedSlice(&slice, &ignored); // Move pointer
      cur_attr_idx++;
    }

    // 2. Read target attr slice
    Slice target_attr_val_slice;
    if (!GetLengthPrefixedSlice(&slice, &target_attr_val_slice))
      return false;
    cur_attr_idx++;
    double val_double;
    bool parsed = false; // To prevent redundant double parse

    // 3. Check all condition for current attr
    while (cur_cond_idx < conditions.size() &&
           conditions[cur_cond_idx].attr_idx == target_attr_idx) {
      const auto& cond = conditions[cur_cond_idx];
      bool match = false;
      const AttrType attr_type = options.attr_types[cond.attr_idx];

      if (attr_type == AttrType::CATEGORICAL) {
        const string& query_val = get<string>(cond.value);
        int cmp = target_attr_val_slice.compare(query_val);
        if (cond.op == CompareOp::EQUAL)
          match = (cmp == 0);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (cmp >= 0);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (cmp <= 0);
        else
          assert(false);
      } else if (attr_type == AttrType::CONTINUOUS) {
        if (!parsed) {
          auto res = std::from_chars(target_attr_val_slice.data(),
                                     target_attr_val_slice.data() +
                                         target_attr_val_slice.size(),
                                     val_double);
          if (res.ec != std::errc())
            return false;
          parsed = true;
        }
        double query_val = std::get<double>(cond.value);
        if (cond.op == CompareOp::EQUAL)
          match = (val_double == query_val);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (val_double >= query_val);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (val_double <= query_val);
      }
      if (!match)
        return false;
      cur_cond_idx++;
    }
  }
  return true;
}
} // namespace bit_lsm