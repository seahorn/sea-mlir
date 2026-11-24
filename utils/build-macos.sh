#!/bin/bash
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Build sea-mlir and SeaHorn dev18 from source on macOS (Apple Silicon or
# x86-64): LLVM + MLIR at the commit of LLVM_REVISION, z3 / Bitwuzla /
# Yices at the pins of docker/Dockerfile, sea-mlir with check-sea, SeaHorn
# at the pins of docker/SeaHorn.Dockerfile against Homebrew's llvm@18, and
# a smoke run (C -> sea export -> sea-vc verdict). docs/BuildMacOS.md is the
# step-by-step guide; `build-macos.sh --help` lists the stages.
#
# Runs with macOS's /bin/bash 3.2: no associative arrays, no mapfile.
set -euo pipefail

usage() {
  cat << 'EOF'
usage: utils/build-macos.sh [options] [stage ...]
  stages (default: all, in this order):
    check      tools, Homebrew packages, Python modules
    llvm       LLVM + MLIR at LLVM_REVISION -> $ROOT/install/llvm
    z3         z3 at Z3_REVISION            -> $ROOT/install/solvers
    bitwuzla   Bitwuzla at BITWUZLA_TAG     -> $ROOT/install/solvers
    yices      Yices at YICES_TAG           -> $ROOT/install/solvers
    sea-mlir   configure, build, check-sea, install -> $ROOT/install/sea-mlir
    seahorn    SeaHorn dev18 at the pins    -> $ROOT/install/seahorn
    smoke      sea-vc on the smoke programs; sea export of C + sea-vc
    report     install names, rpaths, foreign dylibs -> $ROOT/logs/report.txt
  options:
    --root DIR     work directory (default: $SEA_MACOS_ROOT or ~/sea-macos)
    --jobs N       build jobs (default: the number of cores)
    --keep-going   install sea-mlir / SeaHorn even when their tests fail
    --seahorn-z3 shared|separate
                   SeaHorn's z3: the sea-mlir pin (shared) or z3 4.12.0
                   (separate, as the Linux bundle; default)
    --seahorn-yices
                   build SeaHorn with the Yices pin (needs a SeaHorn
                   revision that builds with Yices >= 2.7)
  Every stage logs to $ROOT/logs/<stage>.log and appends its wall time to
  $ROOT/logs/timings.txt. A finished stage is redone when named again.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}
note() { echo "== $*"; }

SRC=$(cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=${SEA_MACOS_ROOT:-$HOME/sea-macos}
JOBS=
KEEP_GOING=0
SEAHORN_Z3=separate
SEAHORN_YICES=0
STAGES=()
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help)
      usage
      exit 0
      ;;
    --root)
      ROOT=$2
      shift
      ;;
    --jobs)
      JOBS=$2
      shift
      ;;
    --keep-going) KEEP_GOING=1 ;;
    --seahorn-z3)
      SEAHORN_Z3=$2
      shift
      ;;
    --seahorn-yices) SEAHORN_YICES=1 ;;
    -*) die "unknown option $1 (--help)" ;;
    *) STAGES+=("$1") ;;
  esac
  shift
done
case "$SEAHORN_Z3" in shared | separate) ;; *) die "--seahorn-z3 shared|separate" ;; esac
[ "$(uname -s)" = Darwin ] || die "this script is for macOS; Linux builds use docker/"
[ -n "$JOBS" ] || JOBS=$(sysctl -n hw.ncpu)
if [ ${#STAGES[@]} -eq 0 ]; then
  STAGES=(check llvm z3 bitwuzla yices sea-mlir seahorn smoke report)
fi
mkdir -p "$ROOT/src" "$ROOT/build" "$ROOT/install" "$ROOT/logs"
ROOT=$(cd -- "$ROOT" && pwd)
LOGS=$ROOT/logs
LLVM=$ROOT/install/llvm
SOLVERS=$ROOT/install/solvers
SEAHORN_Z3_PREFIX=$ROOT/install/seahorn-z3
SEA_MLIR=$ROOT/install/sea-mlir
SEAHORN=$ROOT/install/seahorn

# ---------------------------------------------------------------- pins
# One definition each: docker/Dockerfile (solvers), docker/SeaHorn.Dockerfile
# (SeaHorn and its sub-repositories), LLVM_REVISION (LLVM).
arg() { # arg FILE NAME: the default of `ARG NAME=value`
  sed -n "s/^ARG $2=\\([^ ]*\\)\$/\\1/p" "$1" | head -1
}
LLVM_COMMIT=$(sed -n 's/^commit: *//p' "$SRC/LLVM_REVISION")
Z3_REVISION=$(arg "$SRC/docker/Dockerfile" Z3_REVISION)
BITWUZLA_TAG=$(arg "$SRC/docker/Dockerfile" BITWUZLA_TAG)
YICES_TAG=$(arg "$SRC/docker/Dockerfile" YICES_TAG)
SH=$SRC/docker/SeaHorn.Dockerfile
SEAHORN_Z3_REVISION=$(arg "$SH" SEAHORN_Z3_REVISION)
SEAHORN_REVISION=$(arg "$SH" SEAHORN_REVISION)
SEADSA_REVISION=$(arg "$SH" SEADSA_REVISION)
LLVM_SEAHORN_REVISION=$(arg "$SH" LLVM_SEAHORN_REVISION)
CLAM_REVISION=$(arg "$SH" CLAM_REVISION)
CRAB_REVISION=$(arg "$SH" CRAB_REVISION)
for v in LLVM_COMMIT Z3_REVISION BITWUZLA_TAG YICES_TAG SEAHORN_Z3_REVISION \
  SEAHORN_REVISION SEADSA_REVISION LLVM_SEAHORN_REVISION CLAM_REVISION \
  CRAB_REVISION; do
  [ -n "${!v}" ] || die "pin $v not found (docker/Dockerfile, docker/SeaHorn.Dockerfile, LLVM_REVISION)"
done

BREW=$(command -v brew || true)
brew_prefix() { "$BREW" --prefix "$1"; }

# fetch DIR URL REV: a shallow fetch of one commit or tag into DIR
fetch() {
  local dir=$1 url=$2 rev=$3
  if [ ! -d "$dir/.git" ]; then
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
  fi
  if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2> /dev/null || true)" != "$rev" ]; then
    git -C "$dir" fetch -q --depth 1 origin "$rev"
    git -C "$dir" checkout -q --force FETCH_HEAD
  fi
}

# fix_dylibs DIR: every dylib in DIR gets the install name @rpath/<name>,
# its references to other dylibs of DIR become @rpath/<name>, it searches
# @loader_path, and it is signed again (an edited arm64 binary has an
# invalid signature and is killed at load)
fix_dylibs() {
  local dir=$1 lib name dep base
  for lib in "$dir"/*.dylib; do
    [ -f "$lib" ] && [ ! -L "$lib" ] || continue
    name=$(basename "$lib")
    chmod u+w "$lib"
    install_name_tool -id "@rpath/$name" "$lib"
    otool -L "$lib" | tail -n +2 | awk '{print $1}' | while read -r dep; do
      base=$(basename "$dep")
      case "$dep" in @rpath/*) continue ;; esac
      if [ -e "$dir/$base" ]; then
        install_name_tool -change "$dep" "@rpath/$base" "$lib"
      fi
    done
    if ! otool -l "$lib" | grep -A2 LC_RPATH | grep -q '@loader_path$\|@loader_path '; then
      install_name_tool -add_rpath @loader_path "$lib" 2> /dev/null || true
    fi
    codesign -f -s - "$lib" 2> /dev/null
  done
}

# copy_dylibs FROM TO: the dylibs of FROM (files and their symlinks)
copy_dylibs() {
  mkdir -p "$2"
  find "$1" -maxdepth 1 -name '*.dylib' -exec cp -a {} "$2/" \;
}

stage_check() {
  local missing=()
  for t in git cmake ninja meson pkg-config autoconf gperf python3 \
    install_name_tool otool codesign; do
    command -v "$t" > /dev/null || missing+=("$t")
  done
  xcode-select -p > /dev/null 2>&1 || missing+=("xcode-select --install")
  [ -n "$BREW" ] || missing+=("Homebrew (https://brew.sh)")
  if [ -n "$BREW" ]; then
    for f in cmake ninja meson pkg-config autoconf gperf boost gmp mpfr \
      coreutils llvm@18; do
      "$BREW" list --versions "$f" > /dev/null 2>&1 || missing+=("brew:$f")
    done
  fi
  python3 -c 'import yaml' 2> /dev/null || missing+=("python3 module yaml (docs/BuildMacOS.md step 1: venv)")
  command -v lit > /dev/null || missing+=("lit (docs/BuildMacOS.md step 1: venv)")
  if [ ${#missing[@]} -gt 0 ]; then
    printf 'missing: %s\n' "${missing[@]}"
    die "install the missing tools (docs/BuildMacOS.md, step 1)"
  fi
  note "macOS $(sw_vers -productVersion) $(uname -m), $(sysctl -n hw.ncpu) cores, jobs=$JOBS"
  note "Apple clang: $(cc --version | head -1)"
  note "llvm@18: $("$BREW" list --versions llvm@18)"
  note "pins: llvm $LLVM_COMMIT z3 $Z3_REVISION bitwuzla $BITWUZLA_TAG yices $YICES_TAG"
  note "seahorn $SEAHORN_REVISION sea-dsa $SEADSA_REVISION llvm-seahorn $LLVM_SEAHORN_REVISION"
  note "clam $CLAM_REVISION crab $CRAB_REVISION seahorn-z3 $SEAHORN_Z3_REVISION ($SEAHORN_Z3)"
}

stage_llvm() {
  local src=$ROOT/src/llvm-project build=$ROOT/build/llvm
  fetch "$src" https://github.com/llvm/llvm-project.git "$LLVM_COMMIT"
  # Release with assertions, as docker/Dockerfile; the host target only
  # (sea-mlir generates no code); no zstd / libxml2, so the tools depend on
  # nothing from Homebrew through LLVM; two link jobs at a time because
  # linking needs much memory on a laptop
  cmake -S "$src/llvm" -B "$build" -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$LLVM" \
    -DLLVM_ENABLE_PROJECTS=mlir -DLLVM_TARGETS_TO_BUILD=host \
    -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INSTALL_UTILS=ON \
    -DLLVM_ENABLE_ZLIB=ON -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_PARALLEL_LINK_JOBS=2
  ninja -C "$build" -j"$JOBS" install
  rm -rf "$LLVM/share/lit"
  mkdir -p "$LLVM/share"
  cp -R "$src/llvm/utils/lit" "$LLVM/share/lit"
  cp "$SRC/LLVM_REVISION" "$LLVM/LLVM_REVISION"
  "$LLVM/bin/llvm-config" --version --build-mode --assertion-mode
}

# build_z3 REVISION PREFIX
build_z3() {
  local rev=$1 prefix=$2 src build
  src=$ROOT/src/z3-$rev
  build=$ROOT/build/z3-$rev
  fetch "$src" https://github.com/Z3Prover/z3.git "$rev"
  cmake -S "$src" -B "$build" -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_NAME_DIR=@rpath \
    -DZ3_BUILD_LIBZ3_SHARED=ON -DZ3_BUILD_TEST_EXECUTABLES=OFF
  ninja -C "$build" -j"$JOBS" install
  fix_dylibs "$prefix/lib"
  # the binary finds libz3 (install name @rpath/...) in ../lib
  install_name_tool -add_rpath @loader_path/../lib "$prefix/bin/z3" 2> /dev/null || true
  codesign -f -s - "$prefix/bin/z3"
  "$prefix/bin/z3" --version
}

stage_z3() {
  build_z3 "$Z3_REVISION" "$SOLVERS"
  if [ "$SEAHORN_Z3" = separate ]; then
    build_z3 "$SEAHORN_Z3_REVISION" "$SEAHORN_Z3_PREFIX"
  fi
}

stage_bitwuzla() {
  local src=$ROOT/src/bitwuzla
  fetch "$src" https://github.com/bitwuzla/bitwuzla.git "$BITWUZLA_TAG"
  rm -rf "$src/build"
  (
    cd "$src"
    # CaDiCaL and SymFPU come from Bitwuzla's subprojects/*.wrap pins
    PKG_CONFIG_PATH="$(brew_prefix gmp)/lib/pkgconfig:$(brew_prefix mpfr)/lib/pkgconfig" \
      meson setup build --buildtype=release --prefix="$SOLVERS" \
      --libdir=lib -Ddefault_library=shared \
      -Dtesting=disabled -Dunit_testing=disabled
    ninja -C build -j"$JOBS" install
  )
  mkdir -p "$SOLVERS/share/pins"
  cp "$src/subprojects/cadical.wrap" "$src/subprojects/symfpu.wrap" "$SOLVERS/share/pins/"
  fix_dylibs "$SOLVERS/lib"
}

stage_yices() {
  local src=$ROOT/src/yices2 gmp
  gmp=$(brew_prefix gmp)
  fetch "$src" https://github.com/SRI-CSL/yices2.git "$YICES_TAG"
  (
    cd "$src"
    autoconf
    # without MCSAT, as docker/Dockerfile: GMP only, no LibPoly
    ./configure --prefix="$SOLVERS" CPPFLAGS="-I$gmp/include" LDFLAGS="-L$gmp/lib"
    make -j"$JOBS"
    make install
  )
  fix_dylibs "$SOLVERS/lib"
  "$SOLVERS/bin/yices" --version | head -1
}

stage_sea_mlir() {
  local build=$ROOT/build/sea-mlir rc=0
  # Apple ld (no LLVM_ENABLE_LLD); Boost and GMP from Homebrew; an
  # install that finds its solver dylibs in ../lib
  cmake -S "$SRC" -B "$build" -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DMLIR_DIR="$LLVM/lib/cmake/mlir" -DLLVM_DIR="$LLVM/lib/cmake/llvm" \
    -DLLVM_EXTERNAL_LIT="$LLVM/share/lit/lit.py" -DLLVM_LIT_ARGS="-sv -j$JOBS" \
    -DCMAKE_PREFIX_PATH="$(brew_prefix boost);$(brew_prefix gmp)" \
    -DZ3_ROOT="$SOLVERS" -DSEA_Z3="$SOLVERS/bin/z3" \
    -DBITWUZLA_ROOT="$SOLVERS" \
    -DSEA_ENABLE_YICES=ON -DYICES2_ROOT="$SOLVERS" \
    -DCMAKE_INSTALL_PREFIX="$SEA_MLIR" \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF \
    -DCMAKE_INSTALL_RPATH=@loader_path/../lib
  ninja -C "$build" -j"$JOBS"
  ninja -C "$build" check-sea > "$LOGS/check-sea.log" 2>&1 || rc=$?
  tail -30 "$LOGS/check-sea.log"
  if [ "$rc" != 0 ]; then
    grep -E '^(FAIL|XPASS|UNRESOLVED):' "$LOGS/check-sea.log" > "$LOGS/check-sea-failures.txt" || true
    echo "check-sea failed: $(wc -l < "$LOGS/check-sea-failures.txt") tests, $LOGS/check-sea-failures.txt"
    [ "$KEEP_GOING" = 1 ] || die "check-sea failed (--keep-going installs anyway)"
  fi
  rm -rf "$SEA_MLIR"
  ninja -C "$build" install
  copy_dylibs "$SOLVERS/lib" "$SEA_MLIR/lib"
  cp "$SOLVERS/bin/z3" "$SEA_MLIR/bin/z3"
  "$SEA_MLIR/bin/sea-vc" --version
}

stage_seahorn() {
  local src=$ROOT/src/seahorn build=$ROOT/build/seahorn rc=0 llvm18 z3root
  local yices=(-DCMAKE_DISABLE_FIND_PACKAGE_Yices2=TRUE)
  llvm18=$(brew_prefix llvm@18)
  fetch "$src" https://github.com/seahorn/seahorn.git "$SEAHORN_REVISION"
  fetch "$src/sea-dsa" https://github.com/seahorn/sea-dsa.git "$SEADSA_REVISION"
  fetch "$src/llvm-seahorn" https://github.com/seahorn/llvm-seahorn.git "$LLVM_SEAHORN_REVISION"
  fetch "$src/clam" https://github.com/seahorn/crab-llvm.git "$CLAM_REVISION"
  fetch "$src/clam/crab" https://github.com/seahorn/crab.git "$CRAB_REVISION"
  z3root=$SOLVERS
  [ "$SEAHORN_Z3" = shared ] || z3root=$SEAHORN_Z3_PREFIX
  if [ "$SEAHORN_YICES" = 1 ]; then
    yices=(-DYICES2_HOME="$SOLVERS" -DYICES2_INCLUDE_DIR="$SOLVERS/include"
      -DYICES2_LIBRARY="$SOLVERS/lib/libyices.dylib")
  fi
  # SeaHorn against Homebrew's llvm@18 (the apt.llvm.org 18.1.8 of the
  # Linux build); compiled by Apple clang; Apple ld (SEA_ENABLE_LLD off);
  # clam/crab on (SeaHorn's default)
  cmake -S "$src" -B "$build" -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR="$llvm18/lib/cmake/llvm" \
    -DCMAKE_PREFIX_PATH="$(brew_prefix boost);$(brew_prefix gmp);$(brew_prefix mpfr)" \
    -DZ3_ROOT="$z3root" "${yices[@]}" \
    -DWITH_CLAM=ON -DSEA_ENABLE_LLD=OFF \
    -DCMAKE_INSTALL_PREFIX="$SEAHORN"
  ninja -C "$build" -j"$JOBS"
  rm -rf "$SEAHORN"
  ninja -C "$build" install
  copy_dylibs "$z3root/lib" "$SEAHORN/lib"
  "$SEAHORN/bin/seahorn" --version | head -3
  PATH="$SEAHORN/bin:$llvm18/bin:$PATH" ninja -C "$build" test-all > "$LOGS/seahorn-lit.log" 2>&1 || rc=$?
  tail -15 "$LOGS/seahorn-lit.log"
  if [ "$rc" != 0 ]; then
    [ "$KEEP_GOING" = 1 ] || die "SeaHorn lit failed: $LOGS/seahorn-lit.log"
  fi
}

stage_smoke() {
  local fx=$SRC/docker/smoke work=$ROOT/smoke llvm18 v
  llvm18=$(brew_prefix llvm@18)
  rm -rf "$work"
  mkdir -p "$work"
  cd "$work"
  export PATH="$SEA_MLIR/bin:$SEAHORN/bin:$llvm18/bin:$PATH"
  sea-vc --version
  for solver in z3 yices bitwuzla; do
    sea-vc "$fx/sat.mlir" --lower --solver=$solver > "sat-$solver.log"
    grep -q '^result: sat$' "sat-$solver.log" || die "sat.mlir with $solver: $(cat "sat-$solver.log")"
    grep -q 'mismatches=0' "sat-$solver.log" || die "sat.mlir replay with $solver"
  done
  sea-vc "$fx/unsat.mlir" --config=seam > unsat.log
  grep -q '^result: unsat$' unsat.log || die "unsat.mlir: $(cat unsat.log)"
  # the export frontend: SeaHorn compiles C with llvm@18's clang (-m64:
  # the default -m32 needs 32-bit headers, which macOS does not have)
  for v in sat unsat; do
    sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out="$v.mlir" "$fx/$v.c" > "$v-export.log" 2>&1 ||
      die "sea export of $v.c: $(cat "$v-export.log")"
    grep -q sea.nondet "$v.mlir" || die "$v.mlir has no nondet: the export folded the program"
    sea-vc "$v.mlir" --lower > "$v-vc.log" || true
    grep -q "^result: $v\$" "$v-vc.log" || die "$v.c: $(cat "$v-vc.log")"
  done
  note "smoke: pass"
}

stage_report() {
  local out=$LOGS/report.txt f
  {
    echo "# $(date -u +%Y-%m-%dT%H:%MZ) macOS $(sw_vers -productVersion) $(uname -m)"
    for f in "$SEA_MLIR"/bin/* "$SEA_MLIR"/lib/*.dylib "$SEAHORN"/bin/* "$SEAHORN"/lib/*.dylib; do
      [ -f "$f" ] && [ ! -L "$f" ] || continue
      file -b "$f" | grep -q Mach-O || continue
      echo "## $f"
      otool -L "$f" | tail -n +2
      otool -l "$f" | awk '/LC_RPATH/{getline; getline; print "  rpath " $2}'
    done
  } > "$out"
  note "dylibs outside /usr/lib, /System and @rpath (Homebrew dependencies):"
  grep -E '^\s' "$out" | grep -v -E '^\s+(/usr/lib/|/System/|@rpath/|@loader_path|rpath )' |
    awk '{print $1}' | sort | uniq -c | sort -rn
  note "absolute rpaths (would not survive a move):"
  grep -E 'rpath /' "$out" | sort | uniq -c || true
  note "full report: $out"
}

for s in "${STAGES[@]}"; do
  case "$s" in
    check | llvm | z3 | bitwuzla | yices | sea-mlir | seahorn | smoke | report) ;;
    *) die "unknown stage $s (--help)" ;;
  esac
done
for s in "${STAGES[@]}"; do
  start=$(date +%s)
  note "stage $s (log $LOGS/$s.log)"
  fn=stage_${s//-/_}
  # set -e is ignored inside "if !" and "||", so run the stage in a
  # subshell of its own and read its status from PIPESTATUS
  set +e
  (
    set -e
    "$fn"
  ) 2>&1 | tee "$LOGS/$s.log"
  rc=${PIPESTATUS[0]}
  set -e
  if [ "$rc" != 0 ]; then
    echo "stage $s failed after $(($(date +%s) - start)) s" | tee -a "$LOGS/timings.txt"
    exit 1
  fi
  echo "stage $s: $(($(date +%s) - start)) s jobs=$JOBS" | tee -a "$LOGS/timings.txt"
done
