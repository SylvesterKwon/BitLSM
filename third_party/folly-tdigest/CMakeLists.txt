# folly_tdigest: a self-contained copy of folly::TDigest backed by a minimal
# compatibility-shim header tree, so BitLSM can use t-digest without depending
# on the full Folly library. Provenance and licensing: see NOTICE / LICENSE.

add_library(folly_tdigest STATIC
  src/TDigest.cpp
  src/DoubleRadixSort.cpp
)

target_include_directories(folly_tdigest PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(folly_tdigest PUBLIC cxx_std_20)
