# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Separate LLVM 18 frontend builder. No Yices package is installed or detected.
# Build from the sea-mlir source root; docker/release.py orchestrates the stages.
ARG UBUNTU=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
FROM ${UBUNTU} AS seahorn-base
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git cmake ninja-build clang-18 llvm-18-dev lld-18 \
      libboost-dev libgmp-dev libmpfr-dev zlib1g-dev libzstd-dev \
      libedit-dev libffi-dev libncurses-dev libxml2-dev \
      python3 python3-yaml file patchelf \
  && rm -rf /var/lib/apt/lists/*
ENV CC=clang-18 CXX=clang++-18

# SeaHorn's tested Z3 API version is independent of the sea-mlir solver.
# Keep its SONAME distinct so both libraries can coexist in the bundle.
FROM seahorn-base AS seahorn-z3
ARG JOBS=8
ARG SEAHORN_Z3_REVISION=feda706d0dda2539176246dd221328122c66f77d
RUN set -eux; \
    test "$JOBS" -ge 1 && test "$JOBS" -le 8; \
    mkdir -p /src/z3 && cd /src/z3; \
    git init -q; \
    git remote add origin https://github.com/Z3Prover/z3.git; \
    git fetch -q --depth 1 origin "$SEAHORN_Z3_REVISION"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "$SEAHORN_Z3_REVISION"; \
    cmake -S . -B /build-z3 -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/seahorn-z3 \
      -DZ3_BUILD_LIBZ3_SHARED=ON -DZ3_BUILD_TEST_EXECUTABLES=OFF; \
    cmake --build /build-z3 --parallel "$JOBS" --target install; \
    mkdir -p /opt/seahorn-z3/share/release-licenses; \
    cp LICENSE.txt /opt/seahorn-z3/share/release-licenses/z3-seahorn.txt; \
    printf '%s\n' "$SEAHORN_Z3_REVISION" > /opt/seahorn-z3/source-revision; \
    /opt/seahorn-z3/bin/z3 --version; \
    rm -rf /src/z3 /build-z3

FROM seahorn-z3 AS seahorn
ARG JOBS=8
ARG SEAHORN_REVISION=4f7fdaf30502f745136808aa4cfe1274084d7358
ARG SEADSA_REVISION=3134604191f94f83e1b408a55f404ff23f341011
ARG LLVM_SEAHORN_REVISION=6cfacb810374711b4f7252f412edb2dd23e0f22e
RUN set -eux; \
    mkdir -p /src/seahorn && cd /src/seahorn; \
    git init -q; \
    git remote add origin https://github.com/seahorn/seahorn.git; \
    git fetch -q --depth 1 origin "$SEAHORN_REVISION"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "$SEAHORN_REVISION"; \
    mkdir sea-dsa && cd sea-dsa; \
    git init -q; \
    git remote add origin https://github.com/seahorn/sea-dsa.git; \
    git fetch -q --depth 1 origin "$SEADSA_REVISION"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "$SEADSA_REVISION"; \
    cd ..; mkdir llvm-seahorn && cd llvm-seahorn; \
    git init -q; \
    git remote add origin https://github.com/seahorn/llvm-seahorn.git; \
    git fetch -q --depth 1 origin "$LLVM_SEAHORN_REVISION"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "$LLVM_SEAHORN_REVISION"
COPY docker/build-seahorn-release.sh /usr/local/bin/build-seahorn-release
RUN JOBS="$JOBS" bash /usr/local/bin/build-seahorn-release
ENV PATH=/opt/seahorn/bin:$PATH
WORKDIR /work
