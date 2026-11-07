#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Smoke the delivered tarball, rather than the builder's install tree.
set -eu
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || { echo 'usage: run.sh ARTIFACT_DIR VERSION [--bundle]' >&2; exit 2; }
artifacts=$(CDPATH='' cd -- "$1" && pwd)
version=$2
mode=${3:-}
case "$version" in ''|*[!a-zA-Z0-9._-]*) echo 'invalid version' >&2; exit 2;; esac
case "$mode" in ''|--bundle) ;; *) echo 'expected --bundle' >&2; exit 2;; esac
fixtures=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
package="sea-mlir-$version-linux-x86_64"
[ "$mode" != --bundle ] || package="sea-mlir-bundle-$version-linux-x86_64"
[ -f "$artifacts/$package.tar.gz" ] || { echo "missing $package.tar.gz" >&2; exit 1; }
container="sea-mlir-smoke-$(date +%s)-$$"
cleanup() { docker rm -f "$container" >/dev/null 2>&1 || :; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'smoke-container: %s\n' "$container"
docker run --name "$container" --label sea-mlir.release-smoke=true \
  -v "$artifacts:/artifacts:ro" -v "$fixtures:/fixtures:ro" \
  -e PACKAGE="$package" -e MODE="$mode" \
  ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 \
  sh -ec '
    if [ "$MODE" = --bundle ]; then
      export DEBIAN_FRONTEND=noninteractive
      apt-get update
      apt-get install -y --no-install-recommends clang-18 python3
    fi
    cd /artifacts
    sha256sum -c "$PACKAGE.tar.gz.sha256"
    mkdir /test /work
    tar -xzf "$PACKAGE.tar.gz" -C /test
    cd /work
    sh /fixtures/check.sh "/test/$PACKAGE" "${MODE#--}"
  ' &
client=$!
wait "$client"
