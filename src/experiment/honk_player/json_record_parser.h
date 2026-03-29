#pragma once
#include "bit_lsm.h"
#include "taxi_schema.h"
#include <cstring>
#include <simdjson.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace honk {

using ::Attr;  // global scope (bit_lsm.h)
using bit_lsm::AttrType;
using bit_lsm::BitLSMQuery;
using bit_lsm::CompareOp;
using bit_lsm::QueryCondition;

class RecordParser {
  std::vector<TaxiColumn> columns_;
  std::unordered_map<std::string, uint32_t> col_map_;
  // indexed_indices_: original column indices to extract as attrs.
  // If empty, all columns are indexed (default behavior).
  std::vector<uint32_t> indexed_indices_;
  // reverse_map_: original_col_idx → position in attrs vector.
  // Only populated when indexed_indices_ is non-empty.
  std::unordered_map<uint32_t, uint32_t> reverse_map_;
  uint32_t attr_num_;
  simdjson::ondemand::parser parser_;  // reuses internal buffer across calls

 public:
  explicit RecordParser(const std::vector<uint32_t>& indexed_indices = {})
      : columns_(GetTaxiColumns()),
        col_map_(BuildColumnIndexMap()),
        indexed_indices_(indexed_indices) {
    if (indexed_indices_.empty()) {
      attr_num_ = columns_.size();
    } else {
      attr_num_ = indexed_indices_.size();
      for (uint32_t i = 0; i < indexed_indices_.size(); i++)
        reverse_map_[indexed_indices_[i]] = i;
    }
  }

  /// Append a length-prefixed field to buf: [4B attr_idx][4B len][len bytes]
  static void AppendPayloadField(std::string& buf, uint32_t attr_idx,
                                 const void* data, uint32_t len) {
    buf.append(reinterpret_cast<const char*>(&attr_idx), 4);
    buf.append(reinterpret_cast<const char*>(&len), 4);
    buf.append(reinterpret_cast<const char*>(data), len);
  }

  void ParseRecord(const std::string& json_str,
                   std::vector<Attr>& attrs,
                   std::string& payload) {
    attrs.resize(attr_num_);
    // Set defaults
    if (indexed_indices_.empty()) {
      for (uint32_t i = 0; i < attr_num_; i++) {
        if (columns_[i].type == AttrType::CATEGORICAL)
          attrs[i] = std::string("Null");
        else
          attrs[i] = -1.0;
      }
    } else {
      for (uint32_t i = 0; i < attr_num_; i++) {
        uint32_t orig = indexed_indices_[i];
        if (columns_[orig].type == AttrType::CATEGORICAL)
          attrs[i] = std::string("Null");
        else
          attrs[i] = -1.0;
      }
    }

    payload.clear();

    simdjson::padded_string padded(json_str);
    auto doc = parser_.iterate(padded);

    for (auto field : doc.get_object()) {
      std::string_view key = field.unescaped_key();
      auto found = col_map_.find(std::string(key));
      if (found == col_map_.end()) continue;
      uint32_t orig_idx = found->second;

      auto val = field.value();
      bool is_null = (val.type() == simdjson::ondemand::json_type::null);

      // Extract the value (sentinel for null)
      AttrType atype = columns_[orig_idx].type;
      std::string str_val;
      double dbl_val = -1.0;
      if (is_null) {
        if (atype == AttrType::CATEGORICAL)
          str_val = "Null";
      } else if (atype == AttrType::CATEGORICAL) {
        if (val.type() == simdjson::ondemand::json_type::string) {
          str_val = std::string(val.get_string().value());
        } else {
          auto i64 = val.get_int64();
          if (!i64.error())
            str_val = std::to_string(i64.value());
          else
            str_val = std::to_string(val.get_double().value());
        }
      } else {
        dbl_val = val.get_double().value();
      }

      // Route to attrs (indexed) or payload (non-indexed)
      if (indexed_indices_.empty()) {
        // All indexed
        if (atype == AttrType::CATEGORICAL)
          attrs[orig_idx] = str_val;
        else
          attrs[orig_idx] = dbl_val;
      } else {
        auto it = reverse_map_.find(orig_idx);
        if (it != reverse_map_.end()) {
          // Indexed attr
          if (atype == AttrType::CATEGORICAL)
            attrs[it->second] = str_val;
          else
            attrs[it->second] = dbl_val;
        } else {
          // Non-indexed → length-prefixed payload
          if (atype == AttrType::CATEGORICAL) {
            AppendPayloadField(payload, orig_idx,
                               str_val.data(), str_val.size());
          } else {
            AppendPayloadField(payload, orig_idx,
                               &dbl_val, sizeof(double));
          }
        }
      }
    }
  }
};

struct FilterResult {
  BitLSMQuery query;
  std::string attr_names;
  uint32_t k;
  uint32_t hint_most_selective_attr = UINT32_MAX;  // oracle hint (UINT32_MAX = none)
};

/// Helper: extract a value as Attr from a simdjson value, given the attr type.
inline Attr ExtractValue(simdjson::ondemand::value val, AttrType atype) {
  if (atype == AttrType::CATEGORICAL) {
    if (val.type() == simdjson::ondemand::json_type::string)
      return std::string(val.get_string().value());
    auto i64 = val.get_int64();
    if (!i64.error())
      return std::to_string(i64.value());
    return std::to_string(val.get_double().value());
  }
  return val.get_double().value();
}

inline FilterResult ParseFilters(
    const std::string& json_str,
    const std::vector<TaxiColumn>& columns,
    const std::unordered_map<std::string, uint32_t>& col_map) {

  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(json_str);
  auto doc = parser.iterate(padded);

  std::vector<bit_lsm::OrClause> clause_groups;
  std::vector<std::string> attr_name_list;
  std::unordered_set<std::string> seen_attrs;

  for (auto f : doc["filters"].get_array()) {
    std::string attr_name(f["attr"].get_string().value());
    auto it = col_map.find(attr_name);
    if (it == col_map.end())
      throw std::runtime_error("Unknown filter attr: " + attr_name);
    uint32_t attr_idx = it->second;
    AttrType atype = columns[attr_idx].type;

    if (seen_attrs.insert(attr_name).second)
      attr_name_list.push_back(attr_name);

    std::string op_str(f["op"].get_string().value());

    if (op_str == "eq") {
      bit_lsm::OrClause clause;
      QueryCondition cond;
      cond.attr_idx = attr_idx;
      cond.op = CompareOp::EQUAL;
      cond.value = ExtractValue(f["value"].value(), atype);
      clause.push_back(cond);
      clause_groups.push_back(std::move(clause));
    } else if (op_str == "in") {
      bit_lsm::OrClause clause;
      for (auto v : f["values"].get_array()) {
        QueryCondition cond;
        cond.attr_idx = attr_idx;
        cond.op = CompareOp::EQUAL;
        cond.value = ExtractValue(v.value(), atype);
        clause.push_back(std::move(cond));
      }
      clause_groups.push_back(std::move(clause));
    } else if (op_str == "range") {
      // Lower bound
      {
        bit_lsm::OrClause clause;
        QueryCondition cond;
        cond.attr_idx = attr_idx;
        cond.op = CompareOp::GREATER_EQUAL;
        cond.value = ExtractValue(f["lo"].value(), atype);
        clause.push_back(cond);
        clause_groups.push_back(std::move(clause));
      }
      // Upper bound
      {
        bit_lsm::OrClause clause;
        QueryCondition cond;
        cond.attr_idx = attr_idx;
        cond.op = CompareOp::LESS;
        cond.value = ExtractValue(f["hi"].value(), atype);
        clause.push_back(cond);
        clause_groups.push_back(std::move(clause));
      }
    } else {
      throw std::runtime_error("Unknown filter op: " + op_str);
    }
  }

  std::string names_joined;
  for (size_t i = 0; i < attr_name_list.size(); i++) {
    if (i > 0) names_joined += ",";
    names_joined += attr_name_list[i];
  }

  BitLSMQuery query(std::move(clause_groups));

  // Parse optional oracle hint: most_selective_attr
  uint32_t hint_attr = UINT32_MAX;
  auto hint_field = doc["most_selective_attr"].get_string();
  if (!hint_field.error()) {
    std::string hint_name(hint_field.value());
    auto hit = col_map.find(hint_name);
    if (hit != col_map.end())
      hint_attr = hit->second;
  }

  return {std::move(query), std::move(names_joined),
          static_cast<uint32_t>(attr_name_list.size()), hint_attr};
}

}  // namespace honk
