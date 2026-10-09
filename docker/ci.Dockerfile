# CI base image for BitLSM.
# Provides the toolchain plus the system libraries RocksDB links. RocksDB is
# vendored in-tree and CRoaring is fetched at configure time via CMake
# FetchContent. The PR workflow runs inside this image and builds RocksDB, the
# project and the tests with ccache. Ubuntu 22.04 matches the local dev host
# (gcc 11, cmake 3.22).
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + the system libraries RocksDB links. The set mirrors the root
# CMakeLists, which enables WITH_SNAPPY/LZ4/ZSTD/JEMALLOC and leaves
# zlib/bz2/gflags OFF, so those dev packages are intentionally absent.
# liburing-dev makes RocksDB define ROCKSDB_IOURING_PRESENT; without it the
# scan prefetch queue (block_prefetch_queue.h) has no async read to submit and
# its test skips.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build ccache git ca-certificates \
      libsnappy-dev liblz4-dev libzstd-dev libjemalloc-dev liburing-dev \
    && rm -rf /var/lib/apt/lists/*

# ccache wrappers on PATH so the PR build auto-caches; the workflow caches CCACHE_DIR.
ENV CCACHE_DIR=/ccache
ENV PATH="/usr/lib/ccache:${PATH}"
