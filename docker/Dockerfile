# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# sea-mlir from source on a clean Ubuntu 24.04: LLVM + MLIR at the commit
# of LLVM_REVISION, the solvers at pinned versions, sea-mlir built and
# tested (check-sea), and a runtime image with the installed tools.
# docker/README.md describes the stages; `just docker-build` builds it.
#
#   docker build -f docker/Dockerfile -t sea-mlir .     # z3 + Bitwuzla
#   docker build -f docker/Dockerfile --build-arg YICES=1 --target sea-mlir \
#     -t sea-mlir-test:yices .    # build test with Yices: not a runtime image
#
# The pins: keep docker/README.md ("Pins") in sync.
ARG UBUNTU=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
ARG LLVM_BASE=base
ARG Z3_TAG=z3-5.3.3
ARG BITWUZLA_TAG=0.9.1
ARG YICES_TAG=yices-2.7.0

# ---------------------------------------------------------------- base
# the toolchain of every build stage (clang 18 and lld, as the tested
# builds use)
FROM ${UBUNTU} AS base
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git curl clang-18 lld-18 cmake ninja-build make \
      python3 python3-yaml meson pkg-config autoconf gperf \
      libgmp-dev libmpfr-dev libboost-dev zlib1g-dev libzstd-dev \
  && rm -rf /var/lib/apt/lists/*
ENV CC=clang-18 CXX=clang++-18

# ---------------------------------------------------------------- llvm
# LLVM + MLIR at the commit of LLVM_REVISION (a shallow fetch of that
# commit), Release with assertions, installed to /opt/llvm with the test
# utilities (FileCheck, not, count) and lit (/opt/llvm/share/lit/lit.py).
# Only LLVM_REVISION is copied in: a sea-mlir change keeps this layer.
FROM ${LLVM_BASE} AS llvm
ARG JOBS=8
ARG REUSE_LLVM=0
COPY LLVM_REVISION /opt/llvm/LLVM_REVISION.expected
RUN set -eux; \
    commit="$(sed -n 's/^commit: *//p' /opt/llvm/LLVM_REVISION.expected)"; \
    test -n "$commit"; \
    if [ "$REUSE_LLVM" = 1 ]; then \
      cmp /opt/llvm/LLVM_REVISION.expected /opt/llvm/LLVM_REVISION; \
      test "$(/opt/llvm/bin/llvm-config --build-mode)" = Release; \
      test "$(/opt/llvm/bin/llvm-config --assertion-mode)" = ON; \
      test -f /opt/llvm/lib/cmake/mlir/MLIRConfig.cmake; \
      test -f /opt/llvm/share/lit/lit.py; \
      exit 0; \
    fi; \
    cp /opt/llvm/LLVM_REVISION.expected /opt/llvm/LLVM_REVISION; \
    mkdir /src && cd /src; \
    git init -q llvm-project && cd llvm-project; \
    git remote add origin https://github.com/llvm/llvm-project.git; \
    git fetch -q --depth 1 origin "$commit"; \
    git checkout -q FETCH_HEAD; \
    cmake -S llvm -B /src/build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/llvm \
      -DLLVM_ENABLE_PROJECTS=mlir -DLLVM_TARGETS_TO_BUILD=X86 \
      -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INSTALL_UTILS=ON \
      -DLLVM_ENABLE_LLD=ON -DLLVM_ENABLE_ZLIB=ON -DLLVM_ENABLE_ZSTD=ON \
      -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
      -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF; \
    ninja -C /src/build -j"$JOBS" install; \
    mkdir -p /opt/llvm/share; \
    cp -r llvm/utils/lit /opt/llvm/share/lit; \
    cd / && rm -rf /src

# ---------------------------------------------------------------- solvers
# z3 and Bitwuzla (MIT) in /opt/solvers; Yices (GPLv3) only with
# --build-arg YICES=1, for build tests and solver comparisons: the runtime
# image refuses it (docker/README.md, "Yices").
FROM base AS solvers
ARG JOBS=8
ARG Z3_TAG
ARG BITWUZLA_TAG
RUN set -eux; \
    git clone -q --depth 1 --branch "$Z3_TAG" https://github.com/Z3Prover/z3.git /src/z3; \
    cmake -S /src/z3 -B /src/z3/build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/solvers -DZ3_BUILD_TEST_EXECUTABLES=OFF; \
    ninja -C /src/z3/build -j"$JOBS" install; \
    rm -rf /src/z3
RUN set -eux; \
    git clone -q --depth 1 --branch "$BITWUZLA_TAG" https://github.com/bitwuzla/bitwuzla.git /src/bitwuzla; \
    cd /src/bitwuzla; \
    meson setup build --buildtype=release --prefix=/opt/solvers \
      --libdir=lib -Ddefault_library=shared \
      -Dtesting=disabled -Dunit_testing=disabled; \
    ninja -C build -j"$JOBS" install; \
    cd / && rm -rf /src/bitwuzla
# declared here, so that z3 and Bitwuzla stay cached across YICES=0/1
ARG YICES_TAG
ARG YICES=0
RUN set -eux; \
    if [ "$YICES" = 1 ]; then \
      git clone -q --depth 1 --branch "$YICES_TAG" https://github.com/SRI-CSL/yices2.git /src/yices2; \
      cd /src/yices2; \
      autoconf; \
      ./configure --prefix=/opt/solvers; \
      make -j"$JOBS"; \
      make install; \
      cd / && rm -rf /src/yices2; \
    fi; \
    ls /opt/solvers/lib

# ---------------------------------------------------------------- sea-mlir
# configure against /opt/llvm and /opt/solvers, build, check-sea, install
# to /opt/sea-mlir. The build tree stays in /build (`just docker-check`).
FROM base AS sea-mlir
ARG JOBS=8
ARG YICES=0
ARG SEA_GIT_DESCRIBE=docker
COPY --from=llvm /opt/llvm /opt/llvm
COPY --from=solvers /opt/solvers /opt/solvers
COPY . /src/sea-mlir
RUN set -eux; \
    if [ "$YICES" = 1 ]; then yices=ON; else yices=OFF; fi; \
    cmake -S /src/sea-mlir -B /build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
      -DLLVM_ENABLE_LLD=ON \
      -DMLIR_DIR=/opt/llvm/lib/cmake/mlir \
      -DLLVM_EXTERNAL_LIT=/opt/llvm/share/lit/lit.py \
      -DLLVM_LIT_ARGS="-sv -j4" \
      -DZ3_ROOT=/opt/solvers -DSEA_Z3=/opt/solvers/bin/z3 \
      -DBITWUZLA_ROOT=/opt/solvers \
      -DSEA_ENABLE_YICES="$yices" -DYICES2_ROOT=/opt/solvers \
      -DSEA_GIT_DESCRIBE="$SEA_GIT_DESCRIBE" \
      -DCMAKE_INSTALL_PREFIX=/opt/sea-mlir; \
    ninja -C /build -j"$JOBS"; \
    ninja -C /build -j"$JOBS" check-sea; \
    ninja -C /build install; \
    /opt/sea-mlir/bin/sea-vc --version

# ---------------------------------------------------------------- runtime
# the installed tools and the solver libraries (z3, Bitwuzla), nothing
# else; never with Yices (release binaries omit it: docs/Release.md)
FROM ${UBUNTU} AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgmp10 libgmpxx4ldbl libmpfr6 zlib1g libzstd1 libstdc++6 python3 \
  && rm -rf /var/lib/apt/lists/*
COPY --from=solvers /opt/solvers /opt/solvers
COPY --from=sea-mlir /opt/sea-mlir /opt/sea-mlir
RUN if ls /opt/solvers/lib/libyices* >/dev/null 2>&1; then \
      echo "the runtime image is built without Yices: drop YICES=1" \
           "(use --target sea-mlir for a Yices build test)" >&2; exit 1; \
    fi
ENV PATH=/opt/sea-mlir/bin:/opt/solvers/bin:$PATH
RUN sea-vc --version && ! ldd /opt/sea-mlir/bin/sea-vc | grep 'not found'
WORKDIR /work
CMD ["sea-vc", "--help"]
