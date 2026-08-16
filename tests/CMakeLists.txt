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

# Shared test helpers (oracle + checked wrapper). Links gtest (NOT gtest_main).
add_library(bitlsm_testutil STATIC
  test_util/reference_db.cpp
  test_util/checked_bitlsm.cpp
  test_util/generators.cpp)
target_include_directories(bitlsm_testutil PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bitlsm_testutil PUBLIC bitlsm::bitlsm GTest::gtest)

# 테스트 1개 = 한 줄. <name>.cpp 를 컴파일하고 bitlsm + gtest 를 링크한 뒤
# 각 TEST 케이스를 CTest 에 자동 등록한다.
function(bitlsm_add_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE bitlsm::bitlsm bitlsm_testutil GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 120)
endfunction()

bitlsm_add_test(smoke_test)
bitlsm_add_test(encode_decode_test)
bitlsm_add_test(query_eval_test)
bitlsm_add_test(bitlsm_basic_test)
bitlsm_add_test(bitlsm_query_test)
bitlsm_add_test(sabi_blob_round_trip_test)
bitlsm_add_test(reference_db_test)
bitlsm_add_test(checked_bitlsm_test)
bitlsm_add_test(write_semantics_test)
bitlsm_add_test(binning_boundary_test)
bitlsm_add_test(cross_attr_query_test)
bitlsm_add_test(query_validation_test)
bitlsm_add_test(generators_test)
bitlsm_add_test(randomized_diff_test)
bitlsm_add_test(sst_skip_test)
bitlsm_add_test(okey_encoding_test)
bitlsm_add_test(extractor_test)
bitlsm_add_test(sabi_v5_footer_test)
bitlsm_add_test(okey_zero_regression_test)
bitlsm_add_test(candidate_iterator_test)
bitlsm_add_test(sabi_binning_precision_test)
bitlsm_add_test(sabi_placeholder_binning_test)
bitlsm_add_test(sabi_stats_test)
bitlsm_add_test(estimator_test)
bitlsm_add_test(udi_block_cache_test)
bitlsm_add_test(multiget_skip_test)
bitlsm_add_test(shadow_check_test)
bitlsm_add_test(scan_prefetch_diff_test)
bitlsm_add_test(binning_partition_test)
bitlsm_add_test(ondemand_index_diff_test)
bitlsm_add_test(ondemand_bin_cache_test)

# stress 티어: 큰 시드/스텝 예산. PR CI 는 -LE stress 로 제외하고
# 로컬에서 ctest -L stress 로 실행한다.
function(bitlsm_add_stress_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE bitlsm::bitlsm bitlsm_testutil GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 120 PROPERTIES LABELS stress)
endfunction()

bitlsm_add_stress_test(randomized_diff_stress_test)
