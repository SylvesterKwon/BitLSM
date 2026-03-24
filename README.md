## BitLSM

Based on RocksDB v10.9.0

## Prerequisites

- **Compiler**: GCC 11+
- **CMake**: 3.10+

## Installation

### 1. Initialize Git Submodules

```bash
git submodule update --init --recursive
```

### 2. Install System Dependencies

```bash
sudo apt-get update && sudo apt-get install -y \
    libsnappy-dev \
    zlib1g-dev \
    libbz2-dev \
    liblz4-dev \
    libzstd-dev \
    libjemalloc-dev \
    libssl-dev
```

### 3. Build Facebook Folly

BitLSM uses streaming quantile estimation (TDigest) to determine bin boundaries during bitmap index construction, and relies on [Facebook Folly](https://github.com/facebook/folly) for its TDigest implementation.

Folly and its dependencies are installed under `/opt/BitLSM/installed/`.

```bash
git clone https://github.com/facebook/folly /tmp/folly
cd /tmp/folly
sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive
sudo python3 ./build/fbcode_builder/getdeps.py \
    --allow-system-packages \
    --scratch-path /opt/BitLSM \
    --extra-cmake-defines '{"BUILD_TESTS": "OFF", "BUILD_BENCHMARKS": "OFF"}' \
    --num-jobs 4 \
    build
```

### 4. Build the Project

```bash
cmake -B build
ninja -C build -j$(nproc)
```

### 5. Verify Build

On successful build, executables are generated under `build/bin/`:

```
build/bin/
  BitLSM          # Main executable
  bit-lsm         # BitLSM experiment
  no-index         # No-index baseline experiment
  si-eager         # Secondary Index (Eager) experiment
  si-lu            # Secondary Index (Lazy Update) experiment
  si-ck            # Secondary Index (Compaction Key) experiment
  ldb              # RocksDB ldb tool
  sst_dump         # SST file dump tool
```

## Misc.

### SST dump

```bash
./build/bin/sst_dump <sst_file_path> --command=scan --show_properties
```
