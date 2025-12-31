## LSM-Tree bitmap index
WIP

Based on RocksDB v10.9.0

## Installation
### 0. Install Git Submodule ([RocksDB](https://github.com/facebook/rocksdb) & [CRoaring](https://github.com/RoaringBitmap/CRoaring))
```bash
$ git submodule update --init --recursive
```
TODO: rocksdb 브랜치 설정 하도록 안내
TODO: RoaringBitmap 의존성 소개

### 1. RocksDB 의존성 설치
```bash
$ apt-get update && apt-get install -y \
libsnappy-dev \ 
zlib1g-dev \ 
libbz2-dev \ 
liblz4-dev \ 
libzstd-dev
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
./build/bin/sst_dump /scratch/data/random_bit_props_test/000304.sst --command=scan --show_properties
```