## BitLSM

BitLSM is an LSM-tree storage engine that efficiently supports multi-attribute conjunctive queries.

Built on top of RocksDB v10.10.0.

## Prerequisites

- **Compiler**: GCC 11+
- **CMake**: 3.14+


## Installation

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

`liburing-dev` enables RocksDB's async read path, which
`BitLSMOptions::scan_prefetch_depth` requires. Without it the option is accepted
but has no effect. Needs Linux 5.1 or newer.

If your distribution has no such package, build it:

```bash
git clone --depth 1 --branch liburing-2.5 https://github.com/axboe/liburing
cd liburing && ./configure --prefix="$HOME/.local" && make && make install
```

### 3. Build the Project

```bash
cmake -B build -DCMAKE_PREFIX_PATH="$HOME/.local"   # prefix only if liburing was built by hand
ninja -C build -j$(nproc)
```

## Example

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
      AttrSpec(ORDERED, 8, /*is_signed=*/true, /*is_float=*/false),  // a0: int64
      AttrSpec(ORDERED, 8, /*is_signed=*/true, /*is_float=*/true),   // a1: double
      AttrSpec(UNORDERED),                                           // a2: bytes
  };
  opts.rho = 0.001;

  rocksdb::Options ropts;
  ropts.create_if_missing = true;
  rocksdb::BlockBasedTableOptions topts;

  // 2. Open
  BitLSM db("/tmp/example", opts, ropts, topts);

  // 3. Write, then flush so the row reaches an SST that carries a SABI index
  db.Put("pk-0001", {int64_t(42), 3.14, std::string("foo")}, "payload bytes");
  db.Flush();

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

## Quick Start

### Core Concepts

- **Schema.** A row is a primary key, a fixed set of indexed attributes, and an
opaque payload. Each attribute is `ORDERED` — a native number, range queries —
or `UNORDERED`, opaque bytes matched by equality only.

- **Rho** sets the granularity of the bitmap bins: `rho = 0.1` gives roughly ten
bins per attribute.

- **Queries** are in conjunctive normal form: `BitLSMQuery({{a, b}, {c}})` is
`(a OR b) AND c`.


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

## Testing

Tests use GoogleTest (fetched automatically via CMake `FetchContent`) and are run with CTest.

```bash
# Configure with tests enabled (default ON) and build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBITLSM_BUILD_TESTS=ON
cmake --build build -j

# Run all tests
ctest --test-dir build --output-on-failure

# Run a subset (regex on test name)
ctest --test-dir build -R QueryEval -V
```

Environment knobs for the end-to-end DB tests:

- `MEM_ENV=1` — run against an in-memory RocksDB Env (no disk, faster, fully isolated)
- `TEST_TMPDIR=/path` — redirect the temp DB root (e.g. a tmpfs)
- `KEEP_DB=1` — keep the test DB directory on disk for debugging (prints the path)

Each end-to-end test gets a unique DB directory (`<tmp>/bitlsm_<test>_<pid>`) created and destroyed automatically by the `BitLSMTestBase` fixture, so tests never share on-disk state.

## Code Style

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
