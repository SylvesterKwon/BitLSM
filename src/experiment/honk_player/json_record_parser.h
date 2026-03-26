#pragma once
#include "bit_lsm.h"
#include "taxi_schema.h"
#include <nlohmann/json.hpp>
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
using json = nlohmann::json;

/// Convert any JSON number to string for CATEGORICAL attrs.
inline std::string JsonNumToString(const json& v) {
  if (v.is_number_integer())
    return std::to_string(v.get<int64_t>());
  // Float like 1.0 → truncate to int string if it's a whole number
  double d = v.get<double>();
  auto i = static_cast<int64_t>(d);
  if (static_cast<double>(i) == d)
    return std::to_string(i);
  return std::to_string(d);
}

class RecordParser {
  std::vector<TaxiColumn> columns_;
  std::unordered_map<std::string, uint32_t> col_map_;
  uint32_t attr_num_;

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

    auto j = json::parse(json_str);
    for (auto it = j.begin(); it != j.end(); ++it) {
      auto found = col_map_.find(it.key());
      if (found == col_map_.end()) continue;
      uint32_t idx = found->second;

      if (it.value().is_null()) continue;

      if (columns_[idx].type == AttrType::CATEGORICAL) {
        if (it.value().is_string())
          attrs[idx] = it.value().get<std::string>();
        else
          attrs[idx] = JsonNumToString(it.value());
      } else {
        // CONTINUOUS — all numeric values (int or float) as double
        attrs[idx] = it.value().get<double>();
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

inline FilterResult ParseFilters(
    const std::string& json_str,
    const std::vector<TaxiColumn>& columns,
    const std::unordered_map<std::string, uint32_t>& col_map) {
  auto j = json::parse(json_str);
  auto& filters = j["filters"];

  std::vector<bit_lsm::OrClause> clause_groups;
  std::vector<std::string> attr_name_list;
  std::unordered_set<std::string> seen_attrs;

  for (auto& f : filters) {
    std::string attr_name = f["attr"].get<std::string>();
    auto it = col_map.find(attr_name);
    if (it == col_map.end())
      throw std::runtime_error("Unknown filter attr: " + attr_name);
    uint32_t attr_idx = it->second;
    AttrType atype = columns[attr_idx].type;

    if (seen_attrs.insert(attr_name).second)
      attr_name_list.push_back(attr_name);

    std::string op_str = f["op"].get<std::string>();

    if (op_str == "eq") {
      bit_lsm::OrClause clause;
      QueryCondition cond;
      cond.attr_idx = attr_idx;
      cond.op = CompareOp::EQUAL;
      if (atype == AttrType::CATEGORICAL) {
        if (f["value"].is_string())
          cond.value = f["value"].get<std::string>();
        else
          cond.value = std::to_string(f["value"].get<int64_t>());
      } else {
        cond.value = f["value"].get<double>();
      }
      clause.push_back(cond);
      clause_groups.push_back(std::move(clause));
    } else if (op_str == "in") {
      bit_lsm::OrClause clause;
      for (auto& v : f["values"]) {
        QueryCondition cond;
        cond.attr_idx = attr_idx;
        cond.op = CompareOp::EQUAL;
        if (atype == AttrType::CATEGORICAL) {
          if (v.is_string())
            cond.value = v.get<std::string>();
          else
            cond.value = std::to_string(v.get<int64_t>());
        } else {
          cond.value = v.get<double>();
        }
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
        if (atype == AttrType::CATEGORICAL) {
          if (f["lo"].is_string())
            cond.value = f["lo"].get<std::string>();
          else
            cond.value = std::to_string(f["lo"].get<int64_t>());
        } else {
          cond.value = f["lo"].get<double>();
        }
        clause.push_back(cond);
        clause_groups.push_back(std::move(clause));
      }
      // Upper bound
      {
        bit_lsm::OrClause clause;
        QueryCondition cond;
        cond.attr_idx = attr_idx;
        cond.op = CompareOp::LESS;
        if (atype == AttrType::CATEGORICAL) {
          if (f["hi"].is_string())
            cond.value = f["hi"].get<std::string>();
          else
            cond.value = std::to_string(f["hi"].get<int64_t>());
        } else {
          cond.value = f["hi"].get<double>();
        }
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
