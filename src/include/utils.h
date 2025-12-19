#include <iomanip>
#include <iostream>
#include <string>

#include "rocksdb/db.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/table_properties.h"

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

// Creates random bit property
// TODO: 다중 속성 지원하도록 추가하기 (현재는 하나의 속성에 대한 비트맵만
// 시뮬레이션중)
inline string random_bit_props(size_t bit_props_size) {
  string res(bit_props_size, '0');
  res[rand() % bit_props_size] = '1';
  return res;
}

// Creates initial key-value pairs
inline void create_kvp(DB* db, long long int n, int bit_props_size,
                       int value_size = 32) {
  chrono::_V2::system_clock::time_point start_time, end_time;
  chrono::milliseconds ms_duration;

  cout << "creating " << n << " kvps...\n";
  const long long int batch_size = 1e6;
  long long int total_batch = (n + batch_size - 1) / batch_size;
  long long int auto_increment = 0;
  vector<pair<string, string>> kvps(batch_size);
  WriteOptions wo = WriteOptions();

  start_time = chrono::high_resolution_clock::now();

  for (long long int cur_batch = 0; cur_batch < total_batch; ++cur_batch) {
    if (n - auto_increment < batch_size)
      kvps.resize(n - auto_increment);

    // Creating Batch KVPs
    for (size_t i = 0; i < kvps.size(); i++) {
      kvps[i] = {to_string(auto_increment++), random_bit_props(bit_props_size) +
                                                  '_' +
                                                  random_string(value_size)};
    }

    // RocksDB Put
    WriteBatch batch;
    for (auto& [k, v] : kvps)
      batch.Put(k, v);
    db->Write(wo, &batch);

    cout << "[BATCH " << setw(6) << cur_batch << " / " << setw(6) << total_batch
         << "] ";
    cout << "putted: " << auto_increment << " kvps, elapsed: "
         << chrono::duration_cast<chrono::milliseconds>(
                chrono::high_resolution_clock::now() - start_time)
                .count()
         << "ms \n";
  }

  cout << "created " << n << "kvps. (total:"
       << chrono::duration_cast<chrono::milliseconds>(
              chrono::high_resolution_clock::now() - start_time)
              .count()
       << "ms elpased)\n";
}

// SST 관찰 코드
inline void inspect_sst(const string& sst_file_path) {
  Options options;
  SstFileReader reader(options);

  // 1. SST 파일 열기
  Status s = reader.Open(sst_file_path);
  if (!s.ok()) {
    cerr << "Error opening SST file: " << s.ToString() << "\n";
    return;
  }

  // 2. 속성(Properties) 가져오기
  shared_ptr<const TableProperties> props = reader.GetTableProperties();

  // 3. 디버그할 속성 탐색
  auto it = props->user_collected_properties.find("block_kv_cnt_psum");
  if (it == props->user_collected_properties.end()) {
    cout << "[Error] 'block_kv_cnt_psum' not found in this SST."
         << "\n";
    return;
  }

  // 4. 바이너리 데이터 역직렬화 (복원)
  string data = it->second;
  vector<uint32_t> block_kv_cnt_psum;
  // 데이터 크기가 4바이트(uint32) 단위인지 체크
  if (data.size() % sizeof(uint32_t) != 0) {
    cerr << "[Error] Data size mismatch!" << "\n";
    return;
  }

  block_kv_cnt_psum.resize(data.size() / sizeof(uint32_t));
  memcpy(block_kv_cnt_psum.data(), data.data(), data.size());

  // 5. 출력
  cout << "=== Custom Property Content ===" << "\n";
  cout << "Total Blocks Recorded: " << block_kv_cnt_psum.size() << "\n";
  cout << "Values: [ ";
  for (size_t i = 0; i < block_kv_cnt_psum.size(); ++i) {
    cout << block_kv_cnt_psum[i]
         << (i == block_kv_cnt_psum.size() - 1 ? "" : ", ");
    if (i > 20) { // 너무 많으면 생략
      cout << "... (total " << block_kv_cnt_psum.size() << ")";
      break;
    }
  }
  cout << " ]" << "\n";
}