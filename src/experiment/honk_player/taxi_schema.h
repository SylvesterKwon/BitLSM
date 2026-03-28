#pragma once
#include "bit_lsm.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace honk {

using namespace bit_lsm;

struct TaxiColumn {
  std::string name;
  AttrType type;
};

inline std::vector<TaxiColumn> GetTaxiColumns() {
  return {
      /* 0  */ {"VendorID", AttrType::CATEGORICAL},
      /* 1  */ {"tpep_pickup_datetime", AttrType::CONTINUOUS},
      /* 2  */ {"tpep_dropoff_datetime", AttrType::CONTINUOUS},
      /* 3  */ {"passenger_count", AttrType::CONTINUOUS},
      /* 4  */ {"trip_distance", AttrType::CONTINUOUS},
      /* 5  */ {"RatecodeID", AttrType::CATEGORICAL},
      /* 6  */ {"store_and_fwd_flag", AttrType::CATEGORICAL},
      /* 7  */ {"PULocationID", AttrType::CATEGORICAL},
      /* 8  */ {"DOLocationID", AttrType::CATEGORICAL},
      /* 9  */ {"payment_type", AttrType::CATEGORICAL},
      /* 10 */ {"fare_amount", AttrType::CONTINUOUS},
      /* 11 */ {"extra", AttrType::CONTINUOUS},
      /* 12 */ {"mta_tax", AttrType::CONTINUOUS},
      /* 13 */ {"tip_amount", AttrType::CONTINUOUS},
      /* 14 */ {"tolls_amount", AttrType::CONTINUOUS},
      /* 15 */ {"improvement_surcharge", AttrType::CONTINUOUS},
      /* 16 */ {"total_amount", AttrType::CONTINUOUS},
      /* 17 */ {"congestion_surcharge", AttrType::CONTINUOUS},
      /* 18 */ {"Airport_fee", AttrType::CONTINUOUS},
      /* 19 */ {"cbd_congestion_fee", AttrType::CONTINUOUS},
  };
}

inline std::unordered_map<std::string, uint32_t> BuildColumnIndexMap() {
  auto cols = GetTaxiColumns();
  std::unordered_map<std::string, uint32_t> m;
  for (uint32_t i = 0; i < cols.size(); i++)
    m[cols[i].name] = i;
  return m;
}

/// Build BitLSMOptions for the given indexed column indices.
/// If indexed_indices is empty, all columns are indexed.
inline BitLSMOptions
BuildTaxiBitLSMOptions(const std::vector<uint32_t>& indexed_indices = {}) {
  auto cols = GetTaxiColumns();
  BitLSMOptions opts;
  if (indexed_indices.empty()) {
    opts.attr_num = cols.size();
    for (auto& c : cols)
      opts.attr_types.push_back(c.type);
  } else {
    opts.attr_num = indexed_indices.size();
    for (auto idx : indexed_indices)
      opts.attr_types.push_back(cols[idx].type);
  }
  return opts;
}

} // namespace honk
