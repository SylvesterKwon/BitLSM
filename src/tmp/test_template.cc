#include <chrono>
#include <iostream>
#include <random>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "utils.h"

using namespace std;
using namespace rocksdb;

// CONSTANTS
const string db_path = "/scratch/data";
const string server_address = "0.0.0.0:50051";

// GLOBAL VAR
DB* db;
Options options;
Status status;
chrono::_V2::system_clock::time_point start_time, end_time;
chrono::milliseconds ms_duration;

void test() {}

int main(const int argc, char* argv[]) {
    // configure DB
    options.create_if_missing = true;

    status = DB::Open(options, db_path, &db);
    assert(status.ok());

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    test();
    ////////////////////////////////////////////////////////////////////////////////////////////////////

    // close DB gracefully
    WaitForCompactOptions wait_for_compact_options = WaitForCompactOptions();
    wait_for_compact_options.close_db = true;
    status = db->WaitForCompact(wait_for_compact_options);
    assert(status.ok());
    delete db;
    cout << "DB successfully closed\n";

    return 0;
}
