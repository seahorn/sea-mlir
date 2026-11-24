# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Separate LLVM 18 frontend builder: SeaHorn dev18 with clam/crab, z3 4.12.0.
# No Yices: SeaHorn dev18 does not build with Yices >= 2.7 yet.
# Build from the sea-mlir source root; docker/release.py orchestrates the stages.
ARG UBUNTU=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
FROM ${UBUNTU} AS seahorn-base
ARG DEBIAN_FRONTEND=noninteractive
# LLVM 18 from apt.llvm.org (llvm-toolchain-noble-18, 18.1.8), as SeaHorn's
# CI and the tested local builds use; Ubuntu's own 18.1.3 fails two
# llvm-seahorn InstCombine tests. The key is checked by fingerprint and the
# packages by version, so a changed archive fails the build.
ARG LLVM18_VERSION=1:18.1.8~++20240731025043+3b5b5c1ec4a3-1~exp1~20240731145144.92
ARG LLVM_APT_SIGNER_FINGERPRINT=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
      | gpg --dearmor > /usr/share/keyrings/apt.llvm.org.gpg; \
    gpg --show-keys --fingerprint /usr/share/keyrings/apt.llvm.org.gpg \
      | tr -d ' ' | grep -qx "$LLVM_APT_SIGNER_FINGERPRINT"; \
    echo 'deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-18 main' \
      > /etc/apt/sources.list.d/apt.llvm.org.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      git cmake ninja-build \
      clang-18="$LLVM18_VERSION" llvm-18-dev="$LLVM18_VERSION" \
      lld-18="$LLVM18_VERSION" llvm-18="$LLVM18_VERSION" \
      libboost-dev libgmp-dev libmpfr-dev zlib1g-dev libzstd-dev \
      libedit-dev libffi-dev libncurses-dev libxml2-dev \
      python3 python3-yaml file patchelf; \
    rm -rf /var/lib/apt/lists/*; \
    llvm-config-18 --version | grep -qx 18.1.8
ENV CC=clang-18 CXX=clang++-18

# z3 4.12.0 (feda706), the version SeaHorn dev18 is built and tested with;
# independent of the sea-mlir solver. The sea-mlir pin (z3 master bbd46c9)
# does not serve SeaHorn: Spacer did not finish test/tutorial/add_by_one.c
# -O0 in 120 s (0.5 s with 4.12.0), and units_z3 expr.neg expects 4.12's
# simplifier output (docs/design/BinaryRelease.md).
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
# dev18 of seahorn, sea-dsa and llvm-seahorn; dev18 of clam (crab-llvm) and
# dev of crab, the branches SeaHorn's `extra` target clones
ARG SEAHORN_REVISION=67474bf6d58fe923f5cbb5947def9e794ce613fe
ARG SEADSA_REVISION=1e33f160e8c0b3f395176af019efd7f67f82a79b
ARG LLVM_SEAHORN_REVISION=92258148416984b19111a44b9a2f660f7c4132a6
ARG CLAM_REVISION=3eaf0164d6542962f603d156fd7fcc87c7165129
ARG CRAB_REVISION=49773d0a5740c1fdb37afb15f6f9cc6cfba7b5ae
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
    test "$(git rev-parse HEAD)" = "$LLVM_SEAHORN_REVISION"; \
    cd ..; mkdir clam && cd clam; \
    git init -q; \
    git remote add origin https://github.com/seahorn/crab-llvm.git; \
    git fetch -q --depth 1 origin "$CLAM_REVISION"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "$CLAM_REVISION"; \
    mkdir crab && cd crab; \
    git init -q; \
    git remote add origin https://github.com/seahorn/crab.git; \
    git fetch -q --depth 1 origin "$CRAB_REVISION"; \
    git checkout -q FETCH_HEAD; \
    test "$(git rev-parse HEAD)" = "$CRAB_REVISION"
COPY docker/build-seahorn-release.sh /usr/local/bin/build-seahorn-release
RUN JOBS="$JOBS" bash /usr/local/bin/build-seahorn-release
ENV PATH=/opt/seahorn/bin:$PATH
WORKDIR /work
