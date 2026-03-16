#pragma once

#include "bit_lsm_option.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

struct Schema {
  bit_lsm::BitLSMOptions options; // attr_num, attr_types populated
  uint32_t payload_bytes;

  // Per-attr metadata for data generation
  std::vector<int> cardinalities;    // categorical only (0 for continuous)
  std::vector<double> range_min;     // continuous only
  std::vector<double> range_max;     // continuous only
};

inline Schema load_schema(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open())
    throw std::runtime_error("Cannot open schema file: " + path);

  nlohmann::json j;
  f >> j;

  Schema schema;
  auto& attrs = j.at("attrs");
  schema.options.attr_num = static_cast<uint32_t>(attrs.size());
  schema.payload_bytes = j.value("payload_bytes", 32u);

  for (auto& attr : attrs) {
    std::string type = attr.at("type").get<std::string>();
    if (type == "categorical") {
      schema.options.attr_types.push_back(bit_lsm::AttrType::CATEGORICAL);
      schema.cardinalities.push_back(attr.value("cardinality", 100));
      schema.range_min.push_back(0.0);
      schema.range_max.push_back(0.0);
    } else if (type == "continuous") {
      schema.options.attr_types.push_back(bit_lsm::AttrType::CONTINUOUS);
      schema.cardinalities.push_back(0);
      schema.range_min.push_back(attr.value("min", 0.0));
      schema.range_max.push_back(attr.value("max", 100.0));
    } else {
      throw std::runtime_error("Unknown attr type: " + type);
    }
  }

  return schema;
}
