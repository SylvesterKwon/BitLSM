#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "uniform_generator.h"
#include "zipfian_generator.h"

using namespace std;

class IDatabase {
public:
  virtual void Set(const std::string& key, const std::string& val) = 0;
  virtual std::string Get(const std::string& key) = 0;
};

enum class TestType {
  WriteOnly,
  WriteAndRead,
  // TODO: what else?
};

enum class DistType { UNIFORM, SCRAMBLED_ZIPFIAN };

struct SISpec {
  uint64_t cardinality; // Key space size of this secondary index
  DistType dist;        // Distribution definition
};

void GenerateAndLoad(IDatabase& db, uint64_t num_rows,
                     const vector<SISpec>& si_specs, uint32_t payload_size) {
  cout << "Initializing Generators...\n";

  cout << "NUM_ROWS: " << num_rows << "\n";
  cout << "PAYLOAD_SIZE: " << payload_size << "\n";
  cout << "SI_SPECS: \n";
  for (uint32_t i = 0; i < si_specs.size(); ++i) {
    cout << "  - " << i << ": ";
    cout << si_specs[i].cardinality << ", ";
    cout << (si_specs[i].dist == DistType::UNIFORM ? "uniform"
                                                   : "scrambled_zipfian")
         << "\n";
  }

  // 1. Prepare generator for each SI
  vector<unique_ptr<DistributionGenerator>> generators;
  random_device rd;

  for (const auto& spec : si_specs) {
    if (spec.dist == DistType::SCRAMBLED_ZIPFIAN) {
      generators.push_back(
          make_unique<ScrambledZipfianGenerator>(spec.cardinality, 0.99, rd()));
    } else {
      generators.push_back(
          make_unique<UniformGenerator>(spec.cardinality, rd()));
    }
  }

  // 2. Prepare payload pool
  const string CHARS =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  string payload_pool;
  uint32_t payload_pool_size = payload_size * 1000;
  payload_pool.reserve(payload_pool_size);

  mt19937 rng(rd());
  uniform_int_distribution<int> char_dist(0, CHARS.size() - 1);
  for (int i = 0; i < payload_pool_size; ++i) {
    payload_pool += CHARS[char_dist(rng)];
  }
  uniform_int_distribution<int> pool_start_dist(0, payload_pool.size() -
                                                       payload_size);

  // 3. Generate KVPs
  string value_buffer;
  value_buffer.reserve(256);

  for (uint64_t i = 0; i < num_rows; ++i) {
    // 3-1. Generate PK (auto increment)
    string pk = to_string(i);

    // 3-2. Generate value
    value_buffer.clear();

    // 3-2-1. Generate SI
    for (size_t g = 0; g < generators.size(); ++g) {
      value_buffer += to_string(generators[g]->Next());
      value_buffer += ",";
    }

    // 3-2-2. Generate Payload
    int start_idx = pool_start_dist(rng);
    value_buffer.append(payload_pool, start_idx, payload_size);

    // 3-3. Write request
    db.Set(pk, value_buffer);
  }

  cout << "Loading Complete.\n";
}

void test_zipfian() {
  uint64_t cardinality = 1e5, NUM_REQUESTS = 1e6;
  string filename = "../experiment/distribution_test.csv";

  cout << "Generating " << NUM_REQUESTS << " items from domain " << cardinality
       << "...\n";

  ScrambledZipfianGenerator gen(cardinality, 0.7, 6);
  // ZipfianGenerator gen(cardinality, 0.7);

  ofstream csv_file(filename);
  csv_file << "generated_id" << "\n"; // Header

  for (uint64_t i = 0; i < NUM_REQUESTS; ++i) {
    csv_file << gen.Next() << "\n";
  }

  csv_file.close();
  cout << "Done! Saved to " << filename << endl;
}

int main() {
  test_zipfian();
  // YourDB my_db;

  // uint64_t TOTAL_ROWS = 1000;
  // uint32_t PAYLOAD_BYTES = 64;
  // vector<SISpec> specs = {{100, DistType::UNIFORM},
  //                         {10000, DistType::SCRAMBLED_ZIPFIAN}};

  // GenerateAndLoad(my_db, TOTAL_ROWS, specs, PAYLOAD_BYTES);

  return 0;
}