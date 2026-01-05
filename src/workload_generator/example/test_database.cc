#include "workload_generator/workload_generator.cpp"
#include <cstdint>
#include <fstream>

using namespace std;

class TestDatabase : public IDatabase {
private:
  string get_filename = "get_distribution_test.csv";
  string set_filename = "set_distribution_test.csv";
  ofstream get_csv_file, set_csv_file;
  uint64_t qid = 0;

public:
  // Mock User DB
  vector<IndexSpec> specs = {
      {(uint64_t)1e6, DistType::UNIFORM},       // User Id
      {2, DistType::UNIFORM},                   // Gender
      {250, DistType::UNIFORM},                 // Nationality
      {10, DistType::SCRAMBLED_ZIPFIAN, 0.99}}; // Income level

  TestDatabase() : get_csv_file(get_filename), set_csv_file(set_filename) {
    set_csv_file << "pk,sk_1,sk_2,sk_3,payload\n";
    get_csv_file << "query_id,index_id,query_value\n";
  }
  ~TestDatabase() {
    get_csv_file.close();
    set_csv_file.close();
  }
  void Set(const string& key, const string& val) override {
    set_csv_file << key << "," << val << "\n";
  }
  void Get(const vector<pair<uint32_t, string>>& query,
           vector<pair<string, string>>& results) override {
    for (auto& qi : query) {
      get_csv_file << qid << "," << qi.first << "," << qi.second << "\n";
    }
    qid++;
  }
};

int main() {
  // test_zipfian();
  // YourDB my_db;
  TestDatabase my_db;
  WorkloadGenerator test_wg(my_db, my_db.specs, 32);

  test_wg.Generate((uint64_t)1e6, 0.5, {0.1, 0.3, 0.3, 0.3});
  return 0;
}