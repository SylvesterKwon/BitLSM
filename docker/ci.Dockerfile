# CI base image for BitLSM.
# Installs the system deps + Facebook Folly (via getdeps) into the SAME path
# the project hardcodes (/opt/BitLSM/installed), so the root CMakeLists works
# unchanged. The PR workflow runs inside this image and only builds RocksDB +
# the project + tests (with ccache).
# Ubuntu 22.04 matches the local dev host (gcc 11, cmake 3.22) and avoids
# 24.04's PEP-668 restriction that blocks folly getdeps' `pip install pex`.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + system deps (from README "Install System Dependencies").
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential gcc g++ \
      cmake ninja-build ccache git ca-certificates sudo lsb-release \
      python3 python3-dev python3-venv python3-pip \
      libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev \
      libjemalloc-dev libssl-dev \
      libboost-all-dev libdouble-conversion-dev libevent-dev \
      libgflags-dev libsodium-dev \
    && ln -sf /usr/bin/pip3 /usr/bin/pip \
    && rm -rf /var/lib/apt/lists/*

# Build Folly into /opt/BitLSM/installed via getdeps (same procedure as README).
# FOLLY_REF pins the folly checkout. Default 'main' for the first build; replace
# with the resolved SHA (captured in /opt/folly-built-sha) for reproducible rebuilds.
ARG FOLLY_REF=c8a7db0f856fe54cdb5fb210c7d5fd5716fe67d5
RUN apt-get update \
    && git clone https://github.com/facebook/folly /tmp/folly \
    && cd /tmp/folly \
    && git checkout "${FOLLY_REF}" \
    && git rev-parse HEAD > /opt/folly-built-sha \
    && ./build/fbcode_builder/getdeps.py install-system-deps --recursive \
    && python3 ./build/fbcode_builder/getdeps.py \
         --allow-system-packages \
         --scratch-path /opt/BitLSM \
         --extra-cmake-defines '{"BUILD_TESTS": "OFF", "BUILD_BENCHMARKS": "OFF"}' \
         --num-jobs "$(nproc)" \
         build \
    && rm -rf /tmp/folly /var/lib/apt/lists/* \
         /opt/BitLSM/build /opt/BitLSM/extracted /opt/BitLSM/downloads /opt/BitLSM/repos

# ccache wrappers on PATH so the PR build auto-caches; the workflow caches CCACHE_DIR.
# Set AFTER the folly build so folly's own compiles don't go through ccache.
ENV CCACHE_DIR=/ccache
ENV PATH="/usr/lib/ccache:${PATH}"
