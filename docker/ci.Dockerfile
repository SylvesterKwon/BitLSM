# CI base image for BitLSM.
# Provides the toolchain plus the handful of system libraries RocksDB links;
# RocksDB and folly TDigest are vendored in-tree and CRoaring is fetched at
# configure time via CMake FetchContent, so the image no longer builds Facebook
# Folly. The PR workflow runs inside this image and only builds RocksDB + the
# project + tests (with ccache). Ubuntu 22.04 matches the local dev host
# (gcc 11, cmake 3.22).
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + the system libraries RocksDB links. The set mirrors the root
# CMakeLists, which enables WITH_SNAPPY/LZ4/ZSTD/JEMALLOC and leaves
# zlib/bz2/gflags OFF, so those dev packages are intentionally absent.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build ccache git ca-certificates \
      libsnappy-dev liblz4-dev libzstd-dev libjemalloc-dev \
    && rm -rf /var/lib/apt/lists/*

# ccache wrappers on PATH so the PR build auto-caches; the workflow caches CCACHE_DIR.
ENV CCACHE_DIR=/ccache
ENV PATH="/usr/lib/ccache:${PATH}"
