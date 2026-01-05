#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace std;

class YourDB {
public:
  void set(const string& key, const string& value) {
    // TODO: Replace this
  }
};

// Interface for Distribution Generator
class DistributionGenerator {
public:
  virtual ~DistributionGenerator() = default;
  virtual uint64_t Next() = 0;
};

// Uniform Distribution Generator
class UniformGenerator : public DistributionGenerator {
  mt19937_64 gen;
  uniform_int_distribution<uint64_t> dist;

public:
  UniformGenerator(uint64_t domain_size, uint32_t seed = 42)
      : gen(seed), dist(0, domain_size - 1) {}

  uint64_t Next() override { return dist(gen); }
};

// FNV Hash
inline uint64_t FNVHash64(uint64_t val, uint32_t seed = 42) {
  constexpr uint64_t FNV_OFFSET_BASIS_64 = 0xcbf29ce484222325ULL;
  constexpr uint64_t FNV_PRIME_64 = 1099511628211ULL;

  uint64_t hash = FNV_OFFSET_BASIS_64 ^ seed;
  for (int i = 0; i < 8; i++) {
    uint64_t octet = val & 0xff;
    val >>= 8;
    hash ^= octet;
    hash *= FNV_PRIME_64;
  }
  return hash;
}

// Zipfian Generator
class ZipfianGenerator : public DistributionGenerator {
private:
  uint64_t domain_size;
  double theta;
  double alpha, zetan, eta, zeta2theta;

  mt19937_64 gen;
  uniform_real_distribution<double> dist;

  // Calculate Zeta (1/1^theta + ... + 1/n^theta)
  double Zeta(uint64_t n, double thetaVal) {
    double sum = 0;
    for (uint64_t i = 0; i < n; i++) {
      sum += 1.0 / pow(i + 1, thetaVal);
    }
    return sum;
  }

  // For performance, use Zeta approximation fomular for big N
  // Using Euler-Maclaurin formula
  double ZetaApprox(uint64_t n, double thetaVal) {
    // Handle case when theta is near 1.0. Prevent divided by zero
    if (abs(thetaVal - 1.0) < 1e-9) {
      return log(n + 1);
    }

    // Integral approximation for the general case
    return (pow(n, 1.0 - thetaVal) - 1.0) / (1.0 - thetaVal) + 1.0;
  }

public:
  ZipfianGenerator(uint64_t domain_size, double theta = 0.99,
                   uint32_t seed = 42)
      : domain_size(domain_size), theta(theta), dist(0.0, 1.0) {
    gen.seed(seed);

    zeta2theta = Zeta(2, theta);
    zetan = ZetaApprox(domain_size, theta);
    alpha = 1.0 / (1.0 - theta);
    eta = (1 - pow(2.0 / domain_size, 1 - theta)) / (1 - zeta2theta / zetan);
  }

  uint64_t Next() override {
    double u = dist(gen);
    double uz = u * zetan;

    if (uz < 1.0)
      return 0;
    if (uz < 1.0 + pow(0.5, theta))
      return 1;

    return static_cast<uint64_t>(domain_size * pow(eta * u - eta + 1, alpha));
  }
};

// Scrambled Zipfian Generator
class ScrambledZipfianGenerator : public DistributionGenerator {
private:
  unique_ptr<ZipfianGenerator> zipf_gen;
  uint64_t domain_size;
  uint32_t seed;

public:
  ScrambledZipfianGenerator(uint64_t domain_size, double theta = 0.99,
                            uint32_t seed = 42)
      : domain_size(domain_size), seed(seed) {
    zipf_gen = make_unique<ZipfianGenerator>(domain_size, theta, seed);
  }

  uint64_t Next() override {
    uint64_t rank = zipf_gen->Next();
    return (FNVHash64(rank, seed) % domain_size);
  }
};

enum class DistType { UNIFORM, SCRAMBLED_ZIPFIAN };

struct SISpec {
  uint64_t cardinality; // 키 공간 크기
  DistType dist;        // 분포 타입
};

void GenerateAndLoad(YourDB& db, uint64_t num_rows,
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
    db.set(pk, value_buffer);
  }

  cout << "Loading Complete.\n";
}

void test_zipfian() {
  uint64_t DOMAIN_SIZE = 1e5, NUM_REQUESTS = 1e6;
  string filename = "../../src/experiment/standalone-secondary-index-methods/"
                    "distribution_test.csv";

  cout << "Generating " << NUM_REQUESTS << " items from domain " << DOMAIN_SIZE
       << "...\n";

  ScrambledZipfianGenerator gen(DOMAIN_SIZE, 0.7, 6);
  // ZipfianGenerator gen(DOMAIN_SIZE, 0.7);

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