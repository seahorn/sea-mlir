#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Test a release tarball as a user installs it, on a host that already has
# the runtime packages of docs/Release.md:
#   test-tarball.sh ASSETS standalone|bundle [VERSION]
# ASSETS holds the release's files (the tarball, its .sha256 and its .json
# manifest; other files are ignored). Checks the SHA-256 file, extracts the
# tarball into a new directory whose path has a space, and runs check.sh
# from an empty working directory. VERSION, when given, must be the
# version in the file names. Used by .github/workflows/release-test.yml
# and, inside a clean container, by run.sh.
set -eu
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || { echo 'usage: test-tarball.sh ASSETS standalone|bundle [VERSION]' >&2; exit 2; }
assets=$(CDPATH='' cd -- "$1" && pwd)
kind=$2
want=${3:-}
fixtures=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
case "$kind" in
  standalone) pattern='sea-mlir-[0-9]*-linux-x86_64.tar.gz' ;;
  bundle) pattern='sea-mlir-bundle-[0-9]*-linux-x86_64.tar.gz' ;;
  *) echo 'expected standalone or bundle' >&2; exit 2 ;;
esac
# shellcheck disable=SC2086 # the pattern is a glob
set -- "$assets"/$pattern
[ "$#" = 1 ] && [ -f "$1" ] || { echo "expected exactly one $pattern in $assets" >&2; exit 1; }
archive=$(basename -- "$1")
package=${archive%.tar.gz}
version=${package%-linux-x86_64}
version=${version#sea-mlir-}
version=${version#bundle-}
printf 'archive: %s\nversion: %s\n' "$archive" "$version"
if [ -n "$want" ] && [ "$want" != "$version" ]; then
  echo "version $version, expected $want" >&2
  exit 1
fi
[ -f "$assets/$archive.sha256" ] || { echo "missing $archive.sha256" >&2; exit 1; }
[ -f "$assets/$package.json" ] || { echo "missing $package.json" >&2; exit 1; }
grep -q "\"name\": \"$package\"" "$assets/$package.json" ||
  { echo "$package.json names another package" >&2; exit 1; }
(cd "$assets" && sha256sum -c "$archive.sha256")
# outside any checkout, under a name with a space: the tarball is relocatable
root=$(mktemp -d "${TMPDIR:-/tmp}/sea-mlir-test.XXXXXX")
mkdir "$root/install dir" "$root/work"
tar -xzf "$assets/$archive" -C "$root/install dir"
[ -d "$root/install dir/$package/bin" ] || { echo "no $package/bin in $archive" >&2; exit 1; }
printf 'prefix: %s\n' "$root/install dir/$package"
cd "$root/work"
sh "$fixtures/check.sh" "$root/install dir/$package" "$kind"
