#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Build the pinned frontend in docker/SeaHorn.Dockerfile. Unlike SeaHorn's
# general CI builder, this installs no Yices and never builds its tests.
set -euo pipefail

JOBS=${JOBS:-8}
[[ "$JOBS" =~ ^[1-8]$ ]] || { echo 'JOBS must be between 1 and 8' >&2; exit 2; }
source_dir=/src/seahorn
build_dir=/build-seahorn
prefix=/opt/seahorn
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"

cmake -S "$source_dir" -B "$build_dir" -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm \
  -DZ3_ROOT=/opt/seahorn-z3 \
  -DCMAKE_DISABLE_FIND_PACKAGE_Yices2=TRUE \
  -DWITH_CLAM=OFF -DSEA_ENABLE_LLD=ON \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCPACK_GENERATOR=TGZ \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build_dir" --parallel "$JOBS" --target install \
  units_z3 units_type_checker units_hex_dump
"$build_dir/units/units_z3"
"$build_dir/units/units_type_checker"
"$build_dir/units/units_hex_dump"

# The bundle uses sea-mlir's z3 executable; SeaHorn itself links its separately
# versioned library. Do not install a compiler or unversioned solver alias.
rm -f "$prefix/bin/z3"
mkdir -p "$prefix/lib" "$prefix/share/release-licenses" "$prefix/share/seahorn"
cp -L /opt/seahorn-z3/lib/libz3.so.4.12 "$prefix/lib/libz3.so.4.12"
cp /opt/seahorn-z3/share/release-licenses/z3-seahorn.txt "$prefix/share/release-licenses/"
cp "$source_dir/license.txt" "$prefix/share/release-licenses/seahorn.txt"
cp "$source_dir/sea-dsa/license.txt" "$prefix/share/release-licenses/sea-dsa.txt"
cp "$source_dir/llvm-seahorn/LICENSE.TXT" "$prefix/share/release-licenses/llvm-seahorn.txt"
cp /usr/share/doc/llvm-18/copyright "$prefix/share/release-licenses/llvm-18.txt"
cp /usr/share/doc/libboost1.83-dev/copyright "$prefix/share/release-licenses/boost.txt"
cp /usr/share/doc/libgmp10/copyright "$prefix/share/release-licenses/gmp.txt"
cp /usr/share/doc/python3-yaml/copyright "$prefix/share/release-licenses/pyyaml.txt"
# yama only needs PyYAML's pure Python implementation, avoiding a new ABI
# dependency. The interpreter remains a documented runtime dependency.
cp -R /usr/lib/python3/dist-packages/yaml "$prefix/lib/seapy/yaml"
find "$prefix/lib/seapy/yaml" -type f \( -name '*.so' -o -name '*.pyc' \) -delete
find "$prefix/lib/seapy/yaml" -type d -name __pycache__ -empty -delete
cat > "$prefix/share/seahorn/RELEASE-HOLD.txt" <<'NOTICE'
This bundle is built for local validation only. SeaHorn's current license
is included. Distribution is held until the maintainer relicenses SeaHorn.
Clam/Crab and Yices support are disabled. A system clang-18 and Python 3
are required for C/C++ export. The bundle does not include a compiler.
NOTICE
{
  printf 'seahorn: %s\n' "$(git -C "$source_dir" rev-parse HEAD)"
  printf 'sea-dsa: %s\n' "$(git -C "$source_dir/sea-dsa" rev-parse HEAD)"
  printf 'llvm-seahorn: %s\n' "$(git -C "$source_dir/llvm-seahorn" rev-parse HEAD)"
  printf 'z3: %s\n' "$(cat /opt/seahorn-z3/source-revision)"
  printf 'yices: disabled\nclam: disabled\n'
  llvm-config-18 --version
} > "$prefix/share/seahorn/build-pins.txt"

# Preserve the runtime dependency closure for the separate packaging stage.
# Standard C/C++ runtime libraries remain supplied by Ubuntu 24.04.
# ldd reports transitive dependencies as well as direct ones. Capture package
# ownership before replacing their paths with copies in the staged prefix.
package_manifest="$prefix/share/seahorn/runtime-packages.tsv"
printf 'library\toriginal_path\tpackage\tversion\tlicense\n' > "$package_manifest"
declare -A recorded_libraries=()
record_system_library() {
  local library=$1 name=$2 resolved owner package version license copyright candidate
  [[ -z "${recorded_libraries[$name]:-}" ]] || return 0
  recorded_libraries[$name]=1
  case "$library" in
    /opt/seahorn-z3/*|"$prefix"/*) return 0 ;; # Source pins/licenses recorded above.
  esac
  resolved=$(readlink -f "$library")
  owner=''
  # Ubuntu's merged /usr may expose a different path from dpkg's database.
  for candidate in "$library" "$resolved" "/usr${library}" "${resolved#/usr}"; do
    if owner=$(dpkg-query -S "$candidate" 2>/dev/null); then
      owner=${owner%%$'\n'*}
      break
    fi
  done
  if [[ -z "$owner" ]]; then
    printf 'No package provenance for %s\n' "$library" >&2
    return 1
  fi
  package=${owner%%: /*}
  version=$(dpkg-query -W -f='${Version}' "$package")
  copyright="/usr/share/doc/${package%%:*}/copyright"
  license="share/release-licenses/system-${package//:/_}.txt"
  if [[ ! -f "$copyright" ]]; then
    printf 'Missing copyright for %s (%s)\n' "$library" "$package" >&2
    return 1
  fi
  cp -L "$copyright" "$prefix/$license"
  printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$resolved" "$package" "$version" "$license" >> "$package_manifest"
}
: > "$prefix/share/seahorn/ldd-builder.txt"
for binary in "$prefix"/bin/*; do
  file -b "$binary" | grep -q 'ELF.*executable' || continue
  listing=$(ldd "$binary")
  printf '%s\n%s\n' "${binary##*/}:" "$listing" >> "$prefix/share/seahorn/ldd-builder.txt"
  if grep -Eq 'not found|libyices|libpoly' <<< "$listing"; then
    echo 'unresolved or forbidden SeaHorn runtime dependency' >&2
    exit 1
  fi
  while IFS= read -r library; do
    name=${library##*/}
    case "$name" in
      libc.so.*|libm.so.*|libdl.so.*|librt.so.*|libpthread.so.*|libstdc++.so.*|libgcc_s.so.*) continue ;;
    esac
    record_system_library "$library" "$name"
    if [[ "$(readlink -f "$library")" != "$(readlink -m "$prefix/lib/$name")" ]]; then
      cp -L "$library" "$prefix/lib/$name"
    fi
  done < <(awk '/=> \/.* \(/{print $3}' <<< "$listing")
  # shellcheck disable=SC2016 # The dynamic loader expands ORIGIN.
  patchelf --set-rpath '$ORIGIN/../lib' "$binary"
done
for library in "$prefix"/lib/*.so*; do
  [[ -f "$library" ]] || continue
  # shellcheck disable=SC2016 # The dynamic loader expands ORIGIN.
  patchelf --set-rpath '$ORIGIN' "$library"
done
if find "$prefix/bin" "$prefix/lib" \( -iname '*yices*' -o -name 'clang*' \) -print -quit | grep -q .; then
  echo 'compiler or Yices found in frontend prefix' >&2
  exit 1
fi
"$prefix/bin/seahorn" --version
"$prefix/bin/sea" yama --help

# Package the staged prefix, including the dependency/notice additions above.
mkdir -p /artifacts
cpack --config "$build_dir/CPackConfig.cmake" -G TGZ -B /artifacts \
  -D CPACK_INSTALL_CMAKE_PROJECTS= \
  -D "CPACK_INSTALLED_DIRECTORIES=$prefix;/" \
  -D CPACK_PACKAGE_FILE_NAME=seahorn-frontend-linux-x86_64-held
