#include <cassert>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "rocksdb/slice.h"
#include "uniform_generator.h"
#include "zipfian_generator.h"

using namespace std;

class IDatabase {
public:
  virtual void Set(const string& key, const string& val) = 0;
  virtual void Get(const vector<pair<uint32_t, rocksdb::Slice>>& query,
                   vector<pair<string, string>>& results) = 0;
};

enum class DistType { AUTO_INCREMENT, UNIFORM, SCRAMBLED_ZIPFIAN, ZIPFIAN };

// 1. Enum을 문자열로 바꾸는 함수 정의
string ToString(DistType type) {
  switch (type) {
  case DistType::AUTO_INCREMENT:
    return "AUTO_INCREMENT";
  case DistType::UNIFORM:
    return "UNIFORM";
  case DistType::SCRAMBLED_ZIPFIAN:
    return "SCRAMBLED_ZIPFIAN";
  case DistType::ZIPFIAN:
    return "ZIPFIAN";
  default:
    return "UNKNOWN";
  }
}

struct IndexSpec {
  string index_name;
  uint64_t cardinality; // Key space size of this secondary index
  DistType dist;        // Distribution definition
  double theta = 0.99;  // Theta value for Zipfian dist.
};

class WorkloadGenerator {
private:
  IDatabase& db;
  const vector<IndexSpec>& index_specs;
  vector<unique_ptr<DistributionGenerator>> generators;
  mt19937_64 rng;
  uniform_int_distribution<uint64_t> dist;

  const string CHARS =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  const uint32_t payload_size;
  string value_buffer;
  string payload_pool;
  uniform_int_distribution<uint32_t> char_dist;
  uniform_int_distribution<uint32_t> pool_start_dist;

  void Get(const vector<pair<uint32_t, rocksdb::Slice>>& query,
           vector<pair<string, string>>& results) {
    assert(query.size());
    db.Get(query, results);
  }

  void Set() {
    // 1. Generate PK (auto increment)
    string pk = to_string(generators[0]->Next());

    // 2. Generate value (Value structure: [SI_1],[SI_2],...,[PAYLOAD])
    value_buffer.clear();
    // 2-1. Generate SI
    for (size_t g = 1; g < generators.size(); ++g) {
      value_buffer += to_string(generators[g]->Next());
      value_buffer += ",";
    }
    // 2-2. Generate Payload
    uint32_t start_idx = pool_start_dist(rng);
    value_buffer.append(payload_pool, start_idx, payload_size);

    // 3. Send write request
    db.Set(pk, value_buffer);
  }

public:
  enum WorkloadType {
    WriteOnly,
    WriteAndRead,
    // TODO: what else?
  };
  WorkloadGenerator(IDatabase& db, const vector<IndexSpec>& index_specs,
                    const uint32_t payload_size, const uint32_t seed = 42)
      : db(db), index_specs(index_specs), payload_size(payload_size), rng(seed),
        dist(0, UINT64_MAX), char_dist(0, CHARS.size() - 1) {
    cout << "Initializing Generators...\n\n";
    assert(index_specs.size() > 0);

    // Prepare payload pool
    cout << "Preparing payload pool...\n";
    cout << "\tPAYLOAD_SIZE: " << payload_size << "\n";
    value_buffer.reserve(256);
    uint32_t payload_pool_size = payload_size * 1000;
    payload_pool.reserve(payload_pool_size);
    for (uint32_t i = 0; i < payload_pool_size; ++i) {
      payload_pool += CHARS[char_dist(rng)];
    }
    pool_start_dist = uniform_int_distribution<uint32_t>(
        0, payload_pool.size() - payload_size);

    // Prepare generator for each SI
    cout << "\nPreparing index generator...\n";
    cout << "\tINDEX_SPECS: \n";
    for (uint32_t i = 0; i < index_specs.size(); ++i) {
      cout << "\t\t" << (i ? "SI" : "PK") << " (" << i << "): ";
      cout << index_specs[i].cardinality << ", ";
      cout << ToString(index_specs[i].dist) << "\n\n";
    }
    for (const auto& spec : index_specs) {
      if (spec.dist == DistType::SCRAMBLED_ZIPFIAN) {
        generators.push_back(make_unique<ScrambledZipfianGenerator>(
            spec.cardinality, spec.theta, dist(rng)));
      } else if (spec.dist == DistType::ZIPFIAN) {
        generators.push_back(make_unique<ZipfianGenerator>(
            spec.cardinality, spec.theta, dist(rng)));
      } else {
        generators.push_back(
            make_unique<UniformGenerator>(spec.cardinality, dist(rng)));
      }
    }
  }

  void Generate(const uint64_t num_operations, const double read_ratio,
                const vector<double>& query_complexity_dist,
                const vector<double>& index_preference_dist) {
    uint32_t si_cnt = index_specs.size() - 1;
    // #SI+1 and query complexity dist. vector must be same-sized
    assert(query_complexity_dist.size() == si_cnt + 1);
    assert(index_preference_dist.size() == index_specs.size());

    // Since we don't support complex index combined PK+SK,
    // index_preference_dist[0] must be 0
    assert(index_preference_dist[0] == 0);

    cout << "Generating Workload...\n\n";
    cout << "\tNUM_OPERATIONS: " << num_operations << "\n";
    cout << "\tREAD_RATIO: " << read_ratio << "\n";

    uniform_real_distribution<double> std_uniform_dist(0.0, 1.0);
    discrete_distribution<int> query_complexity_discrete_dist(
        query_complexity_dist.begin(), query_complexity_dist.end());
    discrete_distribution<int> index_preference_discrete_dist(
        index_preference_dist.begin(), index_preference_dist.end());

    vector<uint32_t> selected_indices;
    selected_indices.reserve(si_cnt);
    for (uint64_t i = 0; i < num_operations; ++i) {
      if (num_operations > 100 && i % (num_operations / 100) == 0) {
        double progress = (double)i / num_operations * 100.0;
        cout << "Progress: " << (int)progress << "%\n";
      }
      bool is_read = std_uniform_dist(rng) < read_ratio;
      vector<pair<uint32_t, rocksdb::Slice>> query;
      vector<pair<string, string>> result;
      if (is_read) {
        selected_indices.clear();
        // Pick query complexity
        uint32_t query_complexity = query_complexity_discrete_dist(rng);

        // Pick SIs for query
        for (uint32_t j = 0; j < query_complexity; ++j) {
          bool selected = false;
          uint32_t remaining_retries = 100;
          while (!selected && remaining_retries) {
            uint32_t index_no = index_preference_discrete_dist(rng);
            bool exists = false;
            for (uint32_t idx : selected_indices) {
              if (idx == index_no) {
                exists = true;
                break;
              }
            }
            if (!exists) {
              selected_indices.push_back(index_no);
              selected = true;
            }
            --remaining_retries;
          }
          if (!selected)
            cerr << "Warning: Failed to choose index no.\n";
        }
        sort(selected_indices.begin(), selected_indices.end());
        for (auto& selected_index : selected_indices) {
          query.push_back(
              {selected_index,
               rocksdb::Slice(to_string(generators[selected_index]->Next()))});
        }
        Get(query, result);
      } else {
        Set();
      }
    }

    cout << "Test complete.\n";
  }
};