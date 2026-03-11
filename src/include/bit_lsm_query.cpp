#include "bit_lsm_utils.h"
#include "rocksdb/slice.h"
#include <bit_lsm_query.h>

using namespace std;
using namespace rocksdb;

namespace bit_lsm {
// Uses a two-pointer to evaluate whether a given value satisfies the condition
bool BitLSMQuery::CheckCondition(rocksdb::Slice value_slice,
                                 const BitLSMOptions& options) {
  if (conditions.empty())
    return true;

  uint32_t cur_cond_idx = 0;
  std::string_view buffer(value_slice.data(), value_slice.size());

  // For all query condition
  while (cur_cond_idx < conditions.size()) {
    // 1. Get target attribute
    uint32_t target_attr_idx = conditions[cur_cond_idx].attr_idx;
    const AttrType attr_type = options.attr_types[target_attr_idx];
    AttrView attr_val = DecodeAttr(attr_type, buffer, target_attr_idx);

    // 2. Check all condition for current attr
    while (cur_cond_idx < conditions.size() &&
           conditions[cur_cond_idx].attr_idx == target_attr_idx) {
      const auto& cond = conditions[cur_cond_idx];
      bool match = false;

      if (attr_type == AttrType::CATEGORICAL) {
        std::string_view target_str = std::get<std::string_view>(attr_val);
        const string& query_val = get<string>(cond.value);
        int cmp = target_str.compare(query_val);
        if (cond.op == CompareOp::EQUAL)
          match = (cmp == 0);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (cmp >= 0);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (cmp <= 0);
        else if (cond.op == CompareOp::GREATER)
          match = (cmp > 0);
        else if (cond.op == CompareOp::LESS)
          match = (cmp < 0);
        else
          assert(false);
      } else if (attr_type == AttrType::CONTINUOUS) {
        double val_double = std::get<double>(attr_val);
        double query_val = std::get<double>(cond.value);
        if (cond.op == CompareOp::EQUAL)
          match = (val_double == query_val);
        else if (cond.op == CompareOp::GREATER_EQUAL)
          match = (val_double >= query_val);
        else if (cond.op == CompareOp::LESS_EQUAL)
          match = (val_double <= query_val);
        else if (cond.op == CompareOp::GREATER)
          match = (val_double > query_val);
        else if (cond.op == CompareOp::LESS)
          match = (val_double < query_val);
        else
          assert(false);
      }
      if (!match)
        return false;
      cur_cond_idx++;
    }
  }
  return true;
}
} // namespace bit_lsm