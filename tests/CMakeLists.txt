include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# 테스트 1개 = 한 줄. <name>.cpp 를 컴파일하고 bitlsm + gtest 를 링크한 뒤
# 각 TEST 케이스를 CTest 에 자동 등록한다.
function(bitlsm_add_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE bitlsm::bitlsm GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 120)
endfunction()

bitlsm_add_test(smoke_test)
bitlsm_add_test(encode_decode_test)
bitlsm_add_test(query_eval_test)
bitlsm_add_test(bitlsm_basic_test)
bitlsm_add_test(bitlsm_query_test)
bitlsm_add_test(sabi_blob_round_trip_test)
