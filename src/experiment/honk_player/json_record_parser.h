#pragma once
#include "bit_lsm.h"
#include "taxi_schema.h"
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
  uint32_t attr_num_;
  simdjson::ondemand::parser parser_;  // reuses internal buffer across calls

 public:
  RecordParser()
      : columns_(GetTaxiColumns()),
        col_map_(BuildColumnIndexMap()),
        attr_num_(columns_.size()) {}

  void ParseRecord(const std::string& json_str,
                   std::vector<Attr>& attrs,
                   std::string& payload) {
    attrs.resize(attr_num_);
    // Set defaults
    for (uint32_t i = 0; i < attr_num_; i++) {
      if (columns_[i].type == AttrType::CATEGORICAL)
        attrs[i] = std::string("0");
      else
        attrs[i] = 0.0;
    }

    simdjson::padded_string padded(json_str);
    auto doc = parser_.iterate(padded);

    for (auto field : doc.get_object()) {
      std::string_view key = field.unescaped_key();
      auto found = col_map_.find(std::string(key));
      if (found == col_map_.end()) continue;
      uint32_t idx = found->second;

      auto val = field.value();
      if (val.type() == simdjson::ondemand::json_type::null) continue;

      if (columns_[idx].type == AttrType::CATEGORICAL) {
        if (val.type() == simdjson::ondemand::json_type::string) {
          attrs[idx] = std::string(val.get_string().value());
        } else {
          // Number → string (try int first, fallback to double)
          auto i64 = val.get_int64();
          if (!i64.error())
            attrs[idx] = std::to_string(i64.value());
          else
            attrs[idx] = std::to_string(val.get_double().value());
        }
      } else {
        // CONTINUOUS — numeric as double
        attrs[idx] = val.get_double().value();
      }
    }

    payload = json_str;
  }
};

struct FilterResult {
  BitLSMQuery query;
  std::string attr_names;
  uint32_t k;
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

  return {BitLSMQuery(std::move(clause_groups)), std::move(names_joined),
          static_cast<uint32_t>(attr_name_list.size())};
}

}  // namespace honk
