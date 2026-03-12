cmake_minimum_required(VERSION 3.10)

project(LSMBitmapIndex VERSION 0.1 LANGUAGES CXX)

# 빌드 후 실행 파일이 생성될 위치를 'bin' 디렉터리로 지정
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)

# 성능 측정시 아래 라인 활성화 (RelWithDebInfo 와 함께 사용)
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer")

# RocksDB
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS ON CACHE BOOL "" FORCE)
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

# RocksDB compile options
add_compile_options(-fno-rtti) # RTTI(Run-Time Type Information)를 비활성화
add_compile_definitions(
    OS_LINUX
    ROCKSDB_PLATFORM_POSIX
    ROCKSDB_LIB_IO_POSIX
)

## CRoaring
# TODO: CRoaring도 CMake FetchContent로 변경 고려
set(CROARING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CRoaring)
set(ENABLE_ROARING_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/CRoaring)

# Folly
set(DEPS_ROOT "/opt/lsm-bitmap-index/installed")
set(FOLLY_DIR "${DEPS_ROOT}/folly")
file(GLOB FMT_DIR "${DEPS_ROOT}/fmt-*")
list(APPEND CMAKE_PREFIX_PATH 
    "${FOLLY_DIR}"
    "${FMT_DIR}"
)

## CXXOPTS
add_compile_definitions(CXXOPTS_NO_RTTI) # CXXOPTS compile options
include(FetchContent)
FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG        v3.3.1
)
FetchContent_MakeAvailable(cxxopts)

# MAIN_SOURCE, LIB_SOURCES 설정
set(MAIN_SOURCE "src/main.cpp")
file(GLOB_RECURSE LIB_SOURCES "src/include/*.cpp")

# 패키지 검색
find_package(folly CONFIG REQUIRED)
find_package(gflags REQUIRED) # for folly
find_package(Threads REQUIRED)
find_library(JEMALLOC_LIB jemalloc REQUIRED)

# project_base_lib 정적 라이브러리 생성
add_library(project_base_lib STATIC ${LIB_SOURCES})
target_include_directories(project_base_lib PRIVATE
${CMAKE_CURRENT_SOURCE_DIR}/src/include
${ROCKSDB_SOURCE_DIR}
${ROCKSDB_SOURCE_DIR}/include
${CROARING_SOURCE_DIR}/cpp/roaring
)
target_link_libraries(project_base_lib PUBLIC
roaring
rocksdb
Folly::folly
cxxopts::cxxopts
)

# 실행 파일 타겟을 생성
add_executable(${PROJECT_NAME} ${MAIN_SOURCE})

# 헤더 파일 검색 경로를 추가
set(COMMON_INCLUDE_DIRS
${CMAKE_CURRENT_SOURCE_DIR}/src/include # 프로젝트 자체 헤더
${ROCKSDB_SOURCE_DIR} # Internal API
${ROCKSDB_SOURCE_DIR}/include # Public API
${CROARING_SOURCE_DIR}/cpp/roaring
)

# 라이브러리 링크
set(COMMON_LINK_LIBS
project_base_lib    
workload_generator_lib
roaring
rocksdb
Folly::folly
dl
Threads::Threads
${JEMALLOC_LIB}
m
z
bz2
snappy
lz4
zstd
)

target_include_directories(${PROJECT_NAME} PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE /usr/local/lib)
target_link_libraries(${PROJECT_NAME} PRIVATE ${COMMON_LINK_LIBS})

# WORKLOAD GENERATOR UTIL
file(GLOB_RECURSE WORKLOAD_GENERATOR_SOURCES "src/workload_generator/*.cpp")
add_library(workload_generator_lib STATIC ${WORKLOAD_GENERATOR_SOURCES})

# 라이브러리가 사용할 헤더 경로 지정
target_include_directories(workload_generator_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/workload_generator
    ${COMMON_INCLUDE_DIRS}
)

target_link_libraries(workload_generator_lib PUBLIC
    # Threads::Threads 
    # m
)


# EXPERIMENTS
file(GLOB_RECURSE EXPERIMENT_SOURCES "src/experiment/*.cpp")
foreach(EXP_SRC ${EXPERIMENT_SOURCES})
    get_filename_component(EXP_NAME ${EXP_SRC} NAME_WE) # 파일명 추출
    add_executable(${EXP_NAME} ${EXP_SRC}) # 실행 파일 생성
    get_filename_component(EXP_DIR ${EXP_SRC} DIRECTORY) # 실험 파일의 디렉토리 경로

    # Main.cpp과 동일한 환경 설정 (변수 재사용)
    target_include_directories(${EXP_NAME} PRIVATE ${COMMON_INCLUDE_DIRS})
    target_include_directories(${EXP_NAME} PRIVATE 
        ${COMMON_INCLUDE_DIRS} # 공통 헤더
        ${EXP_DIR}             # 실험용 헤더
    )
    target_link_directories(${EXP_NAME} PRIVATE /usr/local/lib)
    target_link_libraries(${EXP_NAME} PRIVATE ${COMMON_LINK_LIBS})
    
    message(STATUS "Experiment executable added: ${EXP_NAME}")
endforeach()