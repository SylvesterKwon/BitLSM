cmake_minimum_required(VERSION 3.14)

project(BitLSM VERSION 0.1 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)

# Build Type & Compiler Flags Preferences
# Default option (RelWithDebInfo)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build." FORCE)
endif()
# Release mode
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
# Debug mode
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
# RelWithDebInfo mode
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -march=native -mtune=native -g -fno-omit-frame-pointer -DNDEBUG")

string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
message(STATUS "=======================================================")
message(STATUS "Current Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Active CXX Flags : ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
message(STATUS "=======================================================")

# RocksDB
# Apply BitLSM's additions onto the pinned vanilla facebook/rocksdb submodule
# (idempotent: re-runs skip an already-patched tree).
find_package(Git REQUIRED)
set(ROCKSDB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb")
set(ROCKSDB_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/rocksdb-bitlsm.patch")
execute_process(
  COMMAND ${GIT_EXECUTABLE} -C ${ROCKSDB_SRC} apply --reverse --check ${ROCKSDB_PATCH}
  RESULT_VARIABLE ROCKSDB_PATCH_REVERSIBLE
  OUTPUT_QUIET ERROR_QUIET)
if(ROCKSDB_PATCH_REVERSIBLE EQUAL 0)
  message(STATUS "RocksDB BitLSM patch: already applied, skipping")
else()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${ROCKSDB_SRC} apply --check ${ROCKSDB_PATCH}
    RESULT_VARIABLE ROCKSDB_PATCH_APPLICABLE
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT ROCKSDB_PATCH_APPLICABLE EQUAL 0)
    message(FATAL_ERROR
      "patches/rocksdb-bitlsm.patch does not apply cleanly onto the pinned RocksDB "
      "submodule. Upstream likely drifted; update the pin or regenerate the patch.")
  endif()
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${ROCKSDB_SRC} apply ${ROCKSDB_PATCH}
    RESULT_VARIABLE ROCKSDB_PATCH_RESULT)
  if(NOT ROCKSDB_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply patches/rocksdb-bitlsm.patch")
  endif()
  message(STATUS "RocksDB BitLSM patch: applied")
endif()

set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
set(WITH_SNAPPY ON CACHE BOOL "" FORCE)
set(WITH_LZ4 ON CACHE BOOL "" FORCE)
set(WITH_ZSTD ON CACHE BOOL "" FORCE)
set(WITH_JEMALLOC ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/rocksdb)
set(ROCKSDB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rocksdb)

## CRoaring
set(CROARING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CRoaring)
set(ENABLE_ROARING_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CRoaring)

# folly TDigest (vendored; replaces the full Folly dependency)
add_subdirectory(third_party/folly-tdigest)

find_package(Threads REQUIRED)
find_library(JEMALLOC_LIB jemalloc REQUIRED)

# project_base_lib 정적 라이브러리 생성
file(GLOB_RECURSE LIB_SOURCES "src/include/*.cpp")
add_library(project_base_lib STATIC ${LIB_SOURCES})
target_compile_options(project_base_lib PUBLIC -fno-rtti)
target_compile_definitions(project_base_lib PUBLIC
    OS_LINUX
    ROCKSDB_PLATFORM_POSIX
    ROCKSDB_LIB_IO_POSIX
)
target_include_directories(project_base_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${ROCKSDB_SOURCE_DIR}
    ${ROCKSDB_SOURCE_DIR}/include
    ${CROARING_SOURCE_DIR}/cpp/roaring
)
target_link_libraries(project_base_lib PUBLIC
    roaring
    rocksdb
    folly_tdigest
)

# 별칭 타겟 (외부 프로젝트에서 bitlsm::bitlsm으로 참조)
add_library(bitlsm::bitlsm ALIAS project_base_lib)

# Tests
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  option(BITLSM_BUILD_TESTS "Build BitLSM unit & integration tests" ON)
else()
  option(BITLSM_BUILD_TESTS "Build BitLSM unit & integration tests" OFF)
endif()
if(BITLSM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
