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

  uint32_t cur_sk_idx = 0;
  uint32_t cur_cond_idx = 0;

  // For all query condition
  while (cur_cond_idx < conditions.size()) {
    uint32_t target_sk_idx = conditions[cur_cond_idx].sk_idx;

    // 1. Skip unnecessary sk
    while (cur_sk_idx < target_sk_idx) {
      // If there's no sk left (maybe new kind of sk is queried) return false
      if (slice.empty())
        return false;
      Slice ignored;
      GetLengthPrefixedSlice(&slice, &ignored); // Move pointer
      cur_sk_idx++;
    }

    // 2. Read target sk slice
    Slice target_sk_val_slice;
    if (!GetLengthPrefixedSlice(&slice, &target_sk_val_slice))
      return false;
    cur_sk_idx++;
    double val_double;
    bool parsed = false; // To prevent redundant double parse

    // 3. Check all condition for current sk
    while (cur_cond_idx < conditions.size() &&
           conditions[cur_cond_idx].sk_idx == target_sk_idx) {
      const auto& cond = conditions[cur_cond_idx];
      bool match = false;
      const SKType sk_type = options.sk_types[cond.sk_idx];

      if (sk_type == SKType::CATEGORICAL) {
        const string& query_val = get<string>(cond.value);
        int cmp = target_sk_val_slice.compare(query_val);
        if (cond.op == CompareOp::EQUAL)
          match = (cmp == 0);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (cmp >= 0);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (cmp <= 0);
        else
          assert(false);
      } else if (sk_type == SKType::CONTINUOUS) {
        if (!parsed) {
          auto res = std::from_chars(target_sk_val_slice.data(),
                                     target_sk_val_slice.data() +
                                         target_sk_val_slice.size(),
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