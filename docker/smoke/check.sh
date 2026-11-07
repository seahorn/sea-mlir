#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Run inside clean Ubuntu with only the extracted archive and these fixtures.
set -eu
prefix=$1
fixtures=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
export PATH="$prefix/bin:$PATH"
unset LD_LIBRARY_PATH
sea-vc --version
for binary in "$prefix"/bin/* "$prefix"/lib/*.so*; do
  [ -f "$binary" ] || continue
  magic=$(od -An -tx1 -N4 "$binary" | tr -d ' \n')
  [ "$magic" = 7f454c46 ] || continue
  dependencies=$(ldd "$binary")
  printf 'ldd: %s\n%s\n' "$binary" "$dependencies"
  if printf '%s\n' "$dependencies" | grep -E 'not found|[Yy]ices'; then
    exit 1
  fi
done
sea-vc "$fixtures/sat.mlir" --lower > sat.log
cat sat.log
grep -q '^result: sat$' sat.log
grep -q '^replay: assert-failed' sat.log
grep -q 'mismatches=0' sat.log
sea-vc "$fixtures/unsat.mlir" --lower --config=seam > unsat.log
cat unsat.log
grep -q '^result: unsat$' unsat.log
if [ "${2:-}" = bundle ]; then
  clang-18 --version
  python3 --version
  sea --help > sea-help.log
  for verdict in sat unsat; do
    if ! sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out="$verdict.mlir" "$fixtures/$verdict.c" > "$verdict-export.log" 2>&1; then
      cat "$verdict-export.log"
      exit 1
    fi
    cat "$verdict-export.log"
    # Both fixtures must reach the backend as symbolic obligations.
    grep -q "sea.nondet" "$verdict.mlir"
    sea-vc "$verdict.mlir" --lower > "$verdict-bundle.log"
    cat "$verdict-bundle.log"
    grep -q "^result: $verdict$" "$verdict-bundle.log"
    if [ "$verdict" = sat ]; then
      grep -q '^replay: assert-failed' "$verdict-bundle.log"
      grep -q 'mismatches=0' "$verdict-bundle.log"
    fi
  done
fi
printf 'smoke: pass\n'
