
#include "bit_lsm.h"
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace rocksdb;

inline const char* char_set =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
inline const size_t max_index = strlen(char_set) - 1;

// Creates random string with given length
inline string random_string(size_t length) {
  string res;
  res.reserve(length);

  for (size_t i = 0; i < length; ++i) {
    res += char_set[rand() % max_index];
  }

  return res;
}

inline void fill_kvp(bit_lsm::BitLSM* db, uint64_t n, uint32_t attr_num,
                     uint32_t payload_size = 32, uint32_t seed = 42,
                     bool debug = false) {
  srand(seed);
  chrono::_V2::system_clock::time_point start_time;

  if (debug)
    cout << "creating " << n << " kvps into BitLSM using Batch API...\n";

  const uint64_t batch_size = 1e6; // 100만 건씩 묶어서 처리
  uint64_t total_batch = (n + batch_size - 1) / batch_size;
  uint64_t auto_increment = 0;

  start_time = chrono::high_resolution_clock::now();

  for (uint64_t cur_batch = 0; cur_batch < total_batch; ++cur_batch) {
    uint64_t current_batch_limit = min(batch_size, n - auto_increment);
    vector<string> pks;
    vector<vector<Attr>> attrs_list;
    vector<string> payloads;

    pks.reserve(current_batch_limit);
    attrs_list.reserve(current_batch_limit);
    payloads.reserve(current_batch_limit);

    for (uint64_t i = 0; i < current_batch_limit; ++i) {
      vector<Attr> attrs(attr_num);
      for (uint32_t j = 0; j < attr_num; ++j) {
        if (j % 2 == 0) {
          attrs[j] = to_string(rand() % 100); // Categorical
        } else {
          attrs[j] = (double)rand() / RAND_MAX * 100.0; // Continuous
        }
      }

      string payload = random_string(payload_size);
      auto_increment++;
      string pk = to_string(auto_increment);

      pks.push_back(std::move(pk));
      attrs_list.push_back(std::move(attrs));
      payloads.push_back(std::move(payload));
    }

    rocksdb::Status s = db->PutBatch(pks, attrs_list, payloads);
    if (!s.ok()) {
      cerr << "❌ PutBatch failed at batch " << cur_batch + 1 << ": "
           << s.ToString() << "\n";
      break;
    }

    if (debug) {
      cout << "[BATCH " << setw(6) << cur_batch + 1 << " / " << setw(6)
           << total_batch << "] ";
      cout << "putted: " << auto_increment << " kvps, elapsed: "
           << chrono::duration_cast<chrono::milliseconds>(
                  chrono::high_resolution_clock::now() - start_time)
                  .count()
           << "ms \n";
    }
  }

  if (debug) {
    cout << "✅ created " << n << " kvps. (total:"
         << chrono::duration_cast<chrono::milliseconds>(
                chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms elapsed)\n";
  }
}

inline void fill_single_kvp(bit_lsm::BitLSM* db, const string& key,
                            uint32_t attr_num, uint32_t payload_size = 32,
                            uint32_t seed = 42, bool debug = false) {
  srand(seed);
  vector<Attr> attrs(attr_num);
  for (uint32_t j = 0; j < attr_num; ++j) {
    if (j % 2 == 0) {
      attrs[j] = to_string(rand() % 100); // Categorical
    } else {
      attrs[j] = (double)rand() / RAND_MAX * 100.0; // Continuous
    }
  }

  string payload = random_string(payload_size);
  rocksdb::Status s = db->Put(key, attrs, payload);
  assert(s.ok());

  if (debug) {
    cout << "  [DEBUG] Putted single KVP -> Key: " << key
         << " (Attributes: " << attr_num << ", Payload size: " << payload.size()
         << " bytes)\n";
  }
}