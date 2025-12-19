cmake_minimum_required(VERSION 3.10)

project(LSMBitmapIndex VERSION 0.1 LANGUAGES CXX)

# 빌드 후 실행 파일이 생성될 위치를 'bin' 디렉터리로 지정
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)


# RocksDB
set(CMAKE_BUILD_TYPE Debug) # 빌드 타입을 Debug로 설정하여 디버깅 심볼을 포함
# set(CMAKE_BUILD_TYPE Release) # 벤치마크시에는 Release 모드로 빌드 필요
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS ON CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)         # 경고 때문에 빌드 멈춤 방지
# Compression libraries
set(WITH_SNAPPY ON CACHE BOOL "" FORCE)
set(WITH_LZ4 ON CACHE BOOL "" FORCE)
set(WITH_ZSTD ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/rocksdb)
set(ROCKSDB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb)

# RocksDB compile options
add_compile_options(-fno-rtti) # RTTI(Run-Time Type Information)를 비활성화
add_compile_definitions(
    OS_LINUX
    ROCKSDB_PLATFORM_POSIX
    ROCKSDB_LIB_IO_POSIX
)

## CRoaring
set(CROARING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CRoaring)
set(ENABLE_ROARING_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CRoaring)


# src 디렉터리 아래의 모든 .cpp 파일을 찾아 SOURCES 변수에 저장
file(GLOB_RECURSE SOURCES "src/*.cpp")

# 실행 파일 타겟을 생성
add_executable(${PROJECT_NAME} ${SOURCES})

# 헤더 파일 검색 경로를 추가
target_include_directories(${PROJECT_NAME} PRIVATE
    # 프로젝트 자체 헤더
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    
    # RocksDB 헤더
    ${ROCKSDB_SOURCE_DIR} # Internal API
    ${ROCKSDB_SOURCE_DIR}/include # Public API

    # CRoaring 헤더
    ${CROARING_SOURCE_DIR}/cpp/roaring
)

target_link_directories(${PROJECT_NAME} PRIVATE /usr/local/lib)

find_package(Threads REQUIRED)

# 라이브러리 링크
target_link_libraries(${PROJECT_NAME} PRIVATE
    rocksdb
    roaring
    dl
    Threads::Threads
    m
    z
    bz2
    snappy
    lz4
    zstd
)