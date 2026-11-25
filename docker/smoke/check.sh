#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Smoke an extracted release tarball on a host that has only the runtime
# packages of docs/Release.md ("Runtime requirements"):
#   check.sh PREFIX [standalone|bundle]
# Run it from an empty working directory outside PREFIX (logs go there).
# Each check prints `check: <name> pass|FAIL`; every check runs. Names in
# $SMOKE_XFAIL (space- or comma-separated) are expected to fail: they
# print XFAIL, or XPASS (an error) when they pass. Exit 0 and
# `smoke: pass` when no check failed unexpectedly.
set -eu
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || { echo 'usage: check.sh PREFIX [standalone|bundle]' >&2; exit 2; }
# the real path: the tools find their data from their own real path
prefix=$(CDPATH='' cd -- "$1" && pwd -P)
mode=${2:-standalone}
case "$mode" in standalone|bundle) ;; *) echo 'expected standalone or bundle' >&2; exit 2;; esac
fixtures=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
export PATH="$prefix/bin:$PATH"
unset LD_LIBRARY_PATH SEA_MLIR_PROFILE_PATH
tools="sea-vc sea-run sea-opt sea-reduce sea-seam-export"
xfail=$(printf '%s' "${SMOKE_XFAIL:-}" | tr ',' ' ')
failed=''
xfailed=''

is_elf() { [ "$(od -An -tx1 -N4 "$1" | tr -d ' \n')" = 7f454c46 ]; }

# every ELF file (by its magic bytes; `file` may be absent) under bin/,
# lib/ and libexec/ resolves its libraries, each inside the prefix or in
# the system's library directories (nothing of the build machine)
check_libs() {
  for dir in bin lib libexec; do
    if [ -d "$prefix/$dir" ]; then find "$prefix/$dir" -type f; fi
  done > elf-candidates.txt
  while IFS= read -r binary; do
    is_elf "$binary" || continue
    if ! dependencies=$(ldd "$binary" 2>&1); then
      case "$dependencies" in
        *'not a dynamic executable'*|*'statically linked'*)
          printf 'ldd: %s: static\n' "$binary"; continue ;;
      esac
      printf 'ldd: %s\n%s\n' "$binary" "$dependencies"
      return 1
    fi
    case "$dependencies" in
      *'statically linked'*) printf 'ldd: %s: static\n' "$binary"; continue ;;
    esac
    printf 'ldd: %s\n%s\n' "$binary" "$dependencies"
    if printf '%s\n' "$dependencies" | grep 'not found'; then
      return 1
    fi
    # `name => path (address)`; the prefix may contain spaces
    printf '%s\n' "$dependencies" | sed -n 's/^.* => \(.*\) (0x[0-9a-f]*)$/\1/p' |
      while IFS= read -r path; do
        case "$path" in
          "$prefix"/*|/lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) ;;
          *) echo "outside the prefix and the system: $path"; exit 1;;
        esac
      done
  done < elf-candidates.txt
}

# every tool runs; one sea-mlir version; the release solvers are linked
check_version() {
  for tool in $tools; do
    "$tool" --version > "$tool.version"
    head -1 "$tool.version"
  done
  cat sea-vc.version
  version=$(sed -n '1s/^sea-vc (sea-mlir \([^ ]*\) .*/\1/p' sea-vc.version)
  [ -n "$version" ]
  for tool in $tools; do
    grep -q "^$tool (sea-mlir $version " "$tool.version"
  done
  grep -Eq '^ *z3 [0-9]' sea-vc.version
  # release binaries link Yices (BSD-3-Clause from 2.8.0) and Bitwuzla
  grep -Eq '^ *yices [0-9]' sea-vc.version
  grep -Eq '^ *bitwuzla [0-9]' sea-vc.version
}

# sea-reduce-test is a Python script
check_reduce_test() {
  sea-reduce-test test --mode result --config=--lower --result sat \
    "$fixtures/sat.mlir" > reduce-test.log 2>&1 && rc=0 || rc=$?
  cat reduce-test.log
  # exit 1: interesting (the sat fixture gives `result sat`)
  [ "$rc" = 1 ]
}

# `--agent` names the docs inside this tree, and the web site
check_agent_docs() {
  agents=$tools
  if command -v python3 > /dev/null; then agents="$agents sea-reduce-test"; fi
  for tool in $agents; do
    "$tool" --agent > "$tool.agent"
    sed -n '1,3p' "$tool.agent"
    grep -qF "DOCS: $prefix/share/doc/sea-mlir (installed" "$tool.agent"
    grep -q '^DOCS-WEB: https://' "$tool.agent"
  done
  test -f "$prefix/share/doc/sea-mlir/docs/agents/index.md"
}

check_sat_z3() {
  sea-vc "$fixtures/sat.mlir" --lower > sat.log
  cat sat.log
  grep -q '^result: sat$' sat.log
  grep -q '^replay: assert-failed' sat.log
  grep -q 'mismatches=0' sat.log
}

# the shipped profile, found from the binary (no SEA_MLIR_PROFILE_PATH)
check_unsat_profile() {
  sea-vc "$fixtures/unsat.mlir" --lower --config=seam > unsat.log
  cat unsat.log
  grep -qxF "vc.config: $prefix/share/sea-mlir/profiles/seam.cfg" unsat.log
  grep -q '^result: unsat$' unsat.log
}

# the unsat fixture needs no query; the sat one reaches the solver
check_sat_yices() {
  sea-vc "$fixtures/sat.mlir" --lower --solver=yices > sat-yices.log
  cat sat-yices.log
  grep -q '^result: sat$' sat-yices.log
  grep -q 'mismatches=0' sat-yices.log
}

check_sat_bitwuzla() {
  sea-vc "$fixtures/sat.mlir" --lower --solver=bitwuzla > sat-bitwuzla.log
  cat sat-bitwuzla.log
  grep -q '^result: sat$' sat-bitwuzla.log
  grep -q 'mismatches=0' sat-bitwuzla.log
}

# no source path of the release build (docker/Dockerfile builds in
# /src/sea-mlir): Tools/no-source-paths.test on the shipped binaries. The
# build directory's path (/build) is checked by that lit test only: too
# generic a string to scan for here.
check_build_paths() {
  found=0
  for tool in $tools; do
    n=$(grep -c -a -F /src/sea-mlir "$prefix/bin/$tool" || :)
    echo "build-paths: $tool /src/sea-mlir x$n"
    [ "$n" = 0 ] || found=1
  done
  [ "$found" = 0 ]
}

check_bundle_tools() {
  clang-18 --version
  python3 --version
  sea --help > sea-help.log
  ldd "$prefix/bin/seahorn" | grep -qF "libyices.so.2.8 => $prefix/bin/../lib/libyices.so.2.8"
}

# C -> SeaHorn export -> sea-vc; both fixtures stay symbolic
check_bundle_export() {
  for verdict in sat unsat; do
    if ! sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out="$verdict.mlir" \
      "$fixtures/$verdict.c" > "$verdict-export.log" 2>&1; then
      cat "$verdict-export.log"
      return 1
    fi
    cat "$verdict-export.log"
    grep -q "sea.nondet" "$verdict.mlir"
    sea-vc "$verdict.mlir" --lower > "$verdict-bundle.log"
    cat "$verdict-bundle.log"
    grep -q "^result: $verdict$" "$verdict-bundle.log"
    if [ "$verdict" = sat ]; then
      grep -q '^replay: assert-failed' "$verdict-bundle.log"
      grep -q 'mismatches=0' "$verdict-bundle.log"
    fi
  done
}

# SeaHorn's own BMC with its Yices backend (the bundle's libyices.so.2.8)
check_bundle_bmc_yices() {
  for verdict in sat unsat; do
    sea bpf -m64 --bmc=mono --horn-bmc-solver=smt-y2 --log=bmc \
      "$fixtures/$verdict.c" > "$verdict-yices.log" 2>&1 || :
    cat "$verdict-yices.log"
    grep -q 'bmc using Yices2' "$verdict-yices.log"
    grep -q "^$verdict\$" "$verdict-yices.log"
  done
}

run() { # name
  name=$1
  printf '== check: %s\n' "$name"
  set +e
  (set -e; "check_$(printf '%s' "$name" | tr - _)")
  rc=$?
  set -e
  expected=pass
  case " $xfail " in *" $name "*) expected=fail;; esac
  if [ "$rc" = 0 ] && [ "$expected" = pass ]; then
    status=pass
  elif [ "$rc" = 0 ]; then
    status=XPASS; failed="$failed $name"
  elif [ "$expected" = fail ]; then
    status=XFAIL; xfailed="$xfailed $name"
  else
    status=FAIL; failed="$failed $name"
  fi
  printf 'check: %s %s\n' "$name" "$status"
}

checks="libs version agent-docs sat-z3 unsat-profile sat-yices sat-bitwuzla build-paths"
if command -v python3 > /dev/null; then
  checks="$checks reduce-test"
else
  # the standalone runtime of docs/Release.md installs no Python
  printf 'check: reduce-test skipped (no python3)\n'
fi
[ "$mode" = standalone ] || checks="$checks bundle-tools bundle-export bundle-bmc-yices"
for name in $xfail; do
  case " $checks " in *" $name "*) ;; *) echo "SMOKE_XFAIL: no check $name" >&2; exit 2;; esac
done
for name in $checks; do run "$name"; done
[ -z "$xfailed" ] || printf 'smoke: xfail:%s\n' "$xfailed"
if [ -n "$failed" ]; then
  printf 'smoke: FAIL%s\n' "$failed"
  exit 1
fi
printf 'smoke: pass\n'
