# BitLSM

BitLSM is an LSM-tree storage engine that efficiently supports multi-attribute conjunctive queries.

Built on top of RocksDB v10.10.0.

## Installation

Requires GCC 11+ (C++20) and CMake 3.14+.

### 1. Initialize Git Submodules

```bash
git submodule update --init --recursive
```

### 2. Install System Dependencies

```bash
sudo apt-get update && sudo apt-get install -y \
    libsnappy-dev \
    liblz4-dev \
    libzstd-dev \
    libjemalloc-dev \
    liburing-dev
```

### 3. Build the Project

```bash
cmake -B build
cmake --build build -j$(nproc)
```

## Quick Start

### Example

```cpp
#include <rocksdb/options.h>
#include <rocksdb/table.h>

#include <iostream>
#include <string>
#include <vector>

#include "bit_lsm.h"

using namespace bit_lsm;

int main() {
  // 1. Schema
  BitLSMOptions opts;
  opts.attr_num = 3;
  opts.attr_specs = {
      AttrSpec(IndexType::kRange, PhysicalType::kInt, 8),        // a0: BIGINT
      AttrSpec(IndexType::kRange, PhysicalType::kFloat, 8),      // a1: DOUBLE
      AttrSpec(IndexType::kEquality, PhysicalType::kVarBinary),  // a2: VARCHAR, = only
  };
  opts.rho = 0.001;

  rocksdb::Options ropts;
  ropts.create_if_missing = true;
  rocksdb::BlockBasedTableOptions topts;

  // 2. Open
  BitLSM db("/tmp/example", opts, ropts, topts);

  // 3. Write
  db.Put("pk-0001", {int64_t(42), 3.14, std::string("foo")}, "payload bytes");
  db.Flush();  // explicit flush

  // 4. Query in CNF: (a0 = 42 OR a0 = 77) AND (a1 >= 3.0)
  BitLSMQuery q({
      {{0, CompareOp::EQUAL, int64_t(42)}, {0, CompareOp::EQUAL, int64_t(77)}},
      {{1, CompareOp::GREATER_EQUAL, 3.0}},
  });

  // 5. Scan the matching rows
  ValueLayout layout(opts);
  auto it = db.NewIterator(q);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string_view value(it->value().data(), it->value().size());
    AttrView a0 = DecodeAttr(layout, value, 0);
    std::string_view payload = DecodePayload(layout, value);
    std::cout << it->key().ToString() << " a0=" << std::get<int64_t>(a0)
              << " payload=" << payload << "\n";
  }
  return it->status().ok() ? 0 : 1;
}
```

### Core Concepts

- **SABI** (SSTable-Attached Bitmap Index) is the bitmap index BitLSM attaches to every SSTable. For each indexed attribute it keeps one bitmap per value bin over the rows of that SSTable, and a query combines the bins to skip rows before reading them.

- **Schema.** A row consists of a primary key, indexed attributes, and a payload. Each indexed attribute has an index type and a physical type. Use `kRange` for attributes that need range predicates and `kEquality` for those that only need point predicates. The physical type is one of `kInt`, `kUint`, `kFloat`, `kBinary` (fixed width), `kVarBinary`. Any combination works.

- **Rho** is the expected fraction of records in an SSTable that a single bin covers, so each attribute gets at most about `1/rho` bins: `rho = 0.001` allows up to 1,000 bins per attribute. Attributes with fewer distinct values use fewer.

- **Queries** are in conjunctive normal form: `BitLSMQuery({{a, b}, {c}})` is `(a OR b) AND c`.

### API
- `BitLSM(path, opts, rocksdb_opts, table_opts)` — open a database
- `Put(pk, attrs, payload)` — write one row
- `PutBatch(pks, attrs_list, payloads)` — write many rows in one batch
- `Delete(pk)` — delete one row by pk
- `Flush()` — force flushing the memtable
- `NewIterator(query)` — iterate the rows matching a query
- `DecodeAttr(layout, value, i)` — decode attribute out of a given value (row)
- `DecodePayload(layout, value)` — decode the payload out of a given value (row)
- `GetInternalDB()` — the underlying RocksDB instance

## Development

Bug reports and feature requests go to [issues](https://github.com/SylvesterKwon/BitLSM/issues). The rest of this section is for those who want to make the change themselves and open a pull request.

### Testing

Tests use GoogleTest (fetched automatically via CMake `FetchContent`) and are run with CTest.

```bash
# Configure with tests enabled (default ON) and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBITLSM_BUILD_TESTS=ON
cmake --build build -j

# Run all tests
ctest --test-dir build --output-on-failure

# Run a subset (regex on test name)
ctest --test-dir build -R QueryEval -V

# Stress tests are labeled `stress`; CI skips them with -LE stress
ctest --test-dir build -L stress
```

Environment knobs for the end-to-end DB tests:

- `MEM_ENV=1` — run against an in-memory RocksDB Env (no disk, faster, fully isolated)
- `TEST_TMPDIR=/path` — redirect the temp DB root (e.g. a tmpfs)
- `KEEP_DB=1` — keep the test DB directory on disk for debugging (prints the path)

### Code Style

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) enforced via [pre-commit](https://pre-commit.com/).

Set up the hook once after cloning:

```bash
pip install pre-commit   # or: pipx install pre-commit
pre-commit install
```

Now every commit auto-formats changed C++ files. To format the whole tree manually:

```bash
pre-commit run --all-files
```

### Patching RocksDB

BitLSM's changes to RocksDB are kept in `patches/rocksdb-bitlsm.patch`. CMake applies it to the submodule at configure time.

To change RocksDB, edit `third_party/rocksdb` directly and regenerate the patch:

```bash
scripts/regen-rocksdb-patch.sh
```

## License

MIT License, see [LICENSE](LICENSE).
