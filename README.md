## SST-Attached Bitmap Index (SABI)
WIP

Based on RocksDB v10.9.0

## Installation

### 0. Install Rocksdb Submodule
```bash
$ git submodule update --init --recursive
```

### 1. RocksDB 의존성 설치
```bash
$ apt-get update && apt-get install -y \
libsnappy-dev \ 
zlib1g-dev \ 
libbz2-dev \ 
liblz4-dev \ 
libzstd-dev
```

### 2. Project build
```bash
$ mkdir build && cd build
```
```bash
$ cmake ../
```
```bash
$ make -j$(nproc)
```
혹은 build.sh 실행 (`$ ./build.sh`)

### 3. 정상 실행 확인
WIP

## Project structure
WIP
### Benchmark
### Utility


/opt/rocksdb/tools/sst_dump

## TODO
[] tools binary로 주지말고 빌드하는 스크립트를 주도록 변경