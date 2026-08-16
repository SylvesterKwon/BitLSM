## BitLSM

BitLSM is an LSM-tree storage engine that efficiently supports multi-attribute conjunctive queries.

Built on top of RocksDB v10.9.0.

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