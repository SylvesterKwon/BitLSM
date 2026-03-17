## BitLSM
WIP

Based on RocksDB v10.9.0

## Installation
### 0. Install Git Submodule ([RocksDB](https://github.com/facebook/rocksdb) & [CRoaring](https://github.com/RoaringBitmap/CRoaring))
```bash
$ git submodule update --init --recursive
```
TODO: rocksdb 브랜치 설정 하도록 안내
TODO: RoaringBitmap 의존성 소개

### 1. 의존성 설치

RocksDB dependency

```bash
$ apt-get update && apt-get install -y \
libsnappy-dev \ 
zlib1g-dev \ 
libbz2-dev \ 
liblz4-dev \ 
libzstd-dev
```

jemalloc
```bash
sudo apt-get install -y libjemalloc-dev
```

### 1-2. Facebook Folly
Reference: https://github.com/facebook/folly?tab=readme-ov-file#build
```bash
# 1. Clone the repo
git clone https://github.com/facebook/folly
cd folly
# 2. Install dependencies
sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive
# 3. Build, using system dependencies if available
sudo python3 ./build/fbcode_builder/getdeps.py --allow-system-packages --scratch-path /opt/BitLSM build
```

- OpenSSL 관련 에러가 난다면
    ```bash
    sudo apt-get install libssl-dev
    ```


TODO: sh로 만들기 (프로젝트 개발 완료후 일괄 설치 스크립트 생성 예정)

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

## Misc.
- SST dump
예시:
```
./build/bin/sst_dump /scratch/random_bit_props_test/000304.sst --command=scan --show_properties
```