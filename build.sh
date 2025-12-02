#!/bin/bash
# 1. build 디렉터리 생성 및 이동
# rm -rf build
mkdir build && cd build

# 2. CMake 실행하여 빌드 시스템 구성 (Makefile 등 생성)
cmake ..

# 3. make로 컴파일 및 링크
make