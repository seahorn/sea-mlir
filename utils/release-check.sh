#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# release-check: the release gate of a sea-mlir checkout (docs/Release.md).
#
#   utils/release-check.sh [BUILD_DIR] [-- <cmake -D args>]
#   just release-check [build=BUILD_DIR]
#
# 1. configure + build a Release tree in BUILD_DIR (default build-rc). A new
#    BUILD_DIR takes its -D settings from the arguments after `--`, else
#    from build-release/CMakeCache.txt (MLIR_DIR, Z3_ROOT, YICES2_ROOT,
#    SEA_ENABLE_YICES, BITWUZLA_ROOT, SEA_Z3, compilers, LLD, lit) when
#    that exists;
# 2. lit (check-sea): every test must pass;
# 3. install to a temporary prefix (removed at exit; SEA_RC_KEEP=1 keeps it);
# 4. smoke-run the installed tools from outside the source tree: every
#    tool's --version, no missing shared library (ldd), one sat program
#    with replay and prints, one unsat program with --seam through the
#    installed `seam` profile, sea-run, sea-opt and sea-reduce-test;
# 5. `bench.py gate smoke --no-record` with the installed sea-vc if a
#    benchmark checkout is connected (bench/work/, `just bench-connect`);
#    skipped with a message otherwise.
# Prints `release-check: pass` and exits 0, or stops at the first failing
# step with `release-check: FAIL <step>` and exits 1.
#
# Shared machines: the build runs under nice with -j${SEA_RC_JOBS:-8}.
set -u -o pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="build-rc"
if [[ $# -gt 0 && "$1" != "--" ]]; then BUILD="$1"; shift; fi
EXTRA=()
if [[ $# -gt 0 && "$1" == "--" ]]; then shift; EXTRA=("$@"); fi
[[ "$BUILD" = /* ]] || BUILD="$SRC/$BUILD"
JOBS="${SEA_RC_JOBS:-8}"

fail() { echo "release-check: FAIL $*"; exit 1; }
step() { echo; echo "== release-check: $*"; }

step "source"
echo "source: $SRC"
echo "git-describe: $(git -C "$SRC" describe --tags --always --dirty --abbrev=12 2>/dev/null || echo unknown)"
echo "llvm-revision: $(head -1 "$SRC/LLVM_REVISION")"
# the pin's age (utils/llvm-drift.py; commits behind with $LLVM_PROJECT):
# a warning past 45 days, a failure past 90
DRIFT="$(python3 "$SRC/utils/llvm-drift.py" --gate 2>&1)"
DRIFT_RC=$?
sed -n -E 's/^llvm\.(pin-date|pin-age-days|behind[a-z-]*|pin-status): /llvm-\1: /p' <<< "$DRIFT"
grep -q '^llvm\.pin-status: warn' <<< "$DRIFT" \
  && echo "release-check: WARNING the LLVM pin is older than 45 days (just llvm-drift)"
[[ $DRIFT_RC -eq 0 ]] || fail "llvm pin older than 90 days (just llvm-drift; docs/Release.md)"

step "configure (Release) in $BUILD"
ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  if [[ ${#EXTRA[@]} -eq 0 && -f "$SRC/build-release/CMakeCache.txt" ]]; then
    echo "settings from $SRC/build-release/CMakeCache.txt"
    while IFS= read -r line; do EXTRA+=("-D$line"); done < <(
      grep -E '^(MLIR_DIR|Z3_ROOT|YICES2_ROOT|SEA_ENABLE_YICES|BITWUZLA_ROOT|SEA_Z3|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|LLVM_ENABLE_LLD|LLVM_EXTERNAL_LIT)(:[A-Z]+)?=' \
        "$SRC/build-release/CMakeCache.txt" | sed -E 's/^([A-Z0-9_]+)(:[A-Z]+)?=/\1=/')
  fi
  ARGS+=(-GNinja)
fi
ARGS+=("${EXTRA[@]}")
printf '  %s\n' "${ARGS[@]}"
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" "${ARGS[@]}" > "$BUILD/release-check.configure.log" 2>&1 \
  || { tail -20 "$BUILD/release-check.configure.log"; fail configure; }
grep -E '^-- (Z3|Yices2|Bitwuzla):' "$BUILD/release-check.configure.log" | sed 's/^-- /  /'

step "build (nice, -j$JOBS)"
nice -n 10 ninja -C "$BUILD" -j"$JOBS" > "$BUILD/release-check.build.log" 2>&1 \
  || { tail -30 "$BUILD/release-check.build.log"; fail build; }
echo "build: ok ($BUILD/release-check.build.log)"

step "lit (check-sea)"
nice -n 10 ninja -C "$BUILD" -j"$JOBS" check-sea > "$BUILD/release-check.lit.log" 2>&1
LIT_RC=$?
grep -E '^(FAIL|XPASS|UNRESOLVED|TIMEOUT):|^  (Passed|Failed|Unsupported|Expectedly Failed|Unresolved|Timed Out)' "$BUILD/release-check.lit.log"
[[ $LIT_RC -eq 0 ]] || fail lit

step "install"
PREFIX="$(mktemp -d "${TMPDIR:-/tmp}/sea-mlir-rc.XXXXXX")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sea-mlir-rc-work.XXXXXX")"
if [[ "${SEA_RC_KEEP:-0}" != 1 ]]; then
  trap 'rm -rf "$PREFIX" "$WORK"' EXIT
fi
cmake --install "$BUILD" --prefix "$PREFIX" > "$WORK/install.log" 2>&1 \
  || { tail -20 "$WORK/install.log"; fail install; }
echo "prefix: $PREFIX"
(cd "$PREFIX" && find . -type f -not -path './share/doc/sea-mlir/docs/*' | sort | sed 's|^\./|  |')
BIN="$PREFIX/bin"
for t in sea-vc sea-run sea-opt sea-reduce sea-seam-export sea-reduce-test; do
  [[ -x "$BIN/$t" ]] || fail "install: no $t"
done
for t in sea-expr-eval sea-expr-rewriter-test; do
  [[ ! -e "$BIN/$t" ]] || fail "install: $t is a test utility, not installed"
done
[[ -f "$PREFIX/share/sea-mlir/profiles/seam.cfg" ]] || fail "install: no profiles"
[[ -f "$PREFIX/share/doc/sea-mlir/docs/Release.md" ]] || fail "install: no docs"

step "smoke: the installed tools (cwd $WORK, no SEA_MLIR_PROFILE_PATH)"
cd "$WORK" || fail smoke
unset SEA_MLIR_PROFILE_PATH
for t in sea-vc sea-run sea-opt sea-reduce sea-seam-export; do
  if ldd "$BIN/$t" | grep -q 'not found'; then
    ldd "$BIN/$t" | grep 'not found'
    fail "smoke: $t misses a shared library"
  fi
  "$BIN/$t" --version > "$WORK/$t.version" 2>&1 || fail "smoke: $t --version"
  head -1 "$WORK/$t.version"
done
sed -n '2,$p' "$WORK/sea-vc.version"

expect() { # file, regex
  grep -qE "$2" "$1" || { cat "$1"; fail "smoke: $(basename "$1"): no /$2/"; }
}

# sat: a counterexample, replayed by the interpreter, with the program's
# prints
"$BIN/sea-vc" "$SRC/test/VC/print/replay.mlir" --lower > "$WORK/sat.out" 2>&1
rc=$?
[[ $rc -eq 0 ]] || { cat "$WORK/sat.out"; fail "smoke: sat run exit $rc"; }
expect "$WORK/sat.out" '^tool\.version: sea-mlir '
expect "$WORK/sat.out" '^result: sat$'
expect "$WORK/sat.out" '^replay: assert-failed$'
expect "$WORK/sat.out" '^print: a\.val = -?[0-9]+ : si64$'
expect "$WORK/sat.out" '^validate: checked=[1-9][0-9]* mismatches=0'
echo "sat:   $(grep -E '^(result|replay|validate):' "$WORK/sat.out" | head -3 | tr '\n' ' ')"

# unsat with SeaM, through the installed profile
"$BIN/sea-vc" "$SRC/test/VC/seam/inb-ok.mlir" --config=seam > "$WORK/unsat.out" 2>&1
rc=$?
[[ $rc -eq 0 ]] || { cat "$WORK/unsat.out"; fail "smoke: unsat run exit $rc"; }
expect "$WORK/unsat.out" "^vc\.config: $PREFIX/share/sea-mlir/profiles/seam\.cfg$"
expect "$WORK/unsat.out" '^seam\.inb: assumed '
expect "$WORK/unsat.out" '^result: unsat$'
echo "unsat: $(grep -E '^(vc\.config|seam\.inb|result):' "$WORK/unsat.out" | tr '\n' ' ')"

# the interpreter on the sat program's counterexample input
"$BIN/sea-run" "$SRC/test/VC/print/replay.mlir" --input=4=100 > "$WORK/run.out" 2>&1 \
  || { cat "$WORK/run.out"; fail "smoke: sea-run"; }
expect "$WORK/run.out" '^status: assert-failed'
echo "run:   $(grep -E '^status:' "$WORK/run.out")"

# sea-opt: parse, verify and lower
"$BIN/sea-opt" "$SRC/test/VC/seam/inb-ok.mlir" --sea-vc-lower > "$WORK/opt.out" 2>&1 \
  || { cat "$WORK/opt.out"; fail "smoke: sea-opt"; }
expect "$WORK/opt.out" 'sea\.program @ok'
echo "opt:   lowered $(grep -c . "$WORK/opt.out") lines"

# sea-reduce-test finds its sibling tools: the sat program is interesting
# for `result sat`
"$BIN/sea-reduce-test" test --mode result --config="--lower" --result sat \
  "$SRC/test/VC/print/replay.mlir" > "$WORK/reduce.out" 2>&1
rc=$?
[[ $rc -eq 1 ]] || { cat "$WORK/reduce.out"; fail "smoke: sea-reduce-test exit $rc (want 1: interesting)"; }
echo "reduce-test: interesting (exit 1)"

step "bench gate smoke"
if [[ -d "$SRC/bench/work" ]] && [[ -n "$(ls -A "$SRC/bench/work" 2>/dev/null)" ]]; then
  (cd "$SRC" && python3 utils/bench.py gate smoke --sea-vc "$BIN/sea-vc" --no-record) \
    > "$WORK/gate.out" 2>&1
  rc=$?
  grep -E '^(sea-vc|suite|jobs|gate|FAIL|slow|solved)' "$WORK/gate.out" | head -40
  [[ $rc -eq 0 ]] || { tail -20 "$WORK/gate.out"; fail "bench gate smoke"; }
else
  echo "bench: skipped (no benchmark checkout in $SRC/bench/work; just bench-connect)"
fi

echo
echo "release-check: pass ($(head -1 "$WORK/sea-vc.version"))"
