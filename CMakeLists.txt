cmake_minimum_required(VERSION 3.10)

project(SstAttachedBitmapIndex VERSION 0.1 LANGUAGES CXX)

# C++ Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 빌드 타입을 Debug로 설정하여 디버깅 심볼을 포함합니다. (-g 플래그)
set(CMAKE_BUILD_TYPE Debug)

# RTTI(Run-Time Type Information)를 비활성화합니다.
add_compile_options(-fno-rtti)

# src 디렉터리 아래의 모든 .cpp 파일을 찾아 SOURCES 변수에 저장합니다.
file(GLOB_RECURSE SOURCES "src/*.cpp")

# 실행 파일 타겟을 생성합니다. 실행 파일 이름은 프로젝트 이름(${PROJECT_NAME})을 사용합니다.
add_executable(${PROJECT_NAME} ${SOURCES})

# 헤더 파일 검색 경로를 추가
# ${RocksDB_INCLUDE_DIRS}: find_package가 찾아준 RocksDB 헤더 경로
# ${CMAKE_CURRENT_SOURCE_DIR}/src: 우리 프로젝트의 소스 디렉터리
target_include_directories(${PROJECT_NAME} PRIVATE
    ${RocksDB_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_link_directories(${PROJECT_NAME} PRIVATE /usr/local/lib)

# 필요한 라이브러리들을 링크
target_link_libraries(${PROJECT_NAME} PRIVATE
    rocksdb
    dl
    pthread
    m
    z
    bz2
    snappy
    lz4
    zstd
)

# 빌드 후 실행 파일이 생성될 위치를 'bin' 디렉터리로 지정
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)