#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Smoke the Homebrew bottles in a fresh brew container: install from the
# bottles only (packaging/homebrew/install-local.sh), brew test, then the
# export flow on docker/smoke: `sea yama -y sea-mlir-export -m64`, sea-vc
# verdicts (sat with replay), and the DOCS root of `sea-vc --agent` under
# the brew prefix.
#   homebrew-smoke.sh ARTIFACTS [CPUSET]
set -euo pipefail
[[ $# -ge 1 && $# -le 2 ]] || { echo 'usage: homebrew-smoke.sh ARTIFACTS [CPUSET]' >&2; exit 2; }
artifacts=$(CDPATH='' cd -- "$1" && pwd)
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
image=$(sed -n 's/^ARG BREW_IMAGE=//p' "$root/docker/Homebrew.Dockerfile")
[[ -n "$image" ]] || { echo 'no BREW_IMAGE in docker/Homebrew.Dockerfile' >&2; exit 2; }
limits=()
[[ -z "${2:-}" ]] || limits=(--cpuset-cpus "$2")
container="sea-mlir-brew-smoke-$(date +%s)-$$"
cleanup() { docker rm -f "$container" > /dev/null 2>&1 || :; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'smoke-container: %s\n' "$container"
docker run --name "$container" --label sea-mlir.release-smoke=true "${limits[@]}" \
  -v "$artifacts:/bottle-test:ro" \
  -v "$root/packaging/homebrew/install-local.sh:/scripts/install-local.sh:ro" \
  -v "$root/docker/smoke:/fixtures:ro" \
  "$image" bash -euo pipefail -c '
    start=$SECONDS
    bash /scripts/install-local.sh /bottle-test seahorn sea-mlir-llvm
    echo "time: install $((SECONDS - start)) s"
    brew list --versions
    brew test seahorn/seahorn/sea-mlir seahorn/seahorn/seahorn seahorn/seahorn/sea-mlir-llvm
    prefix=$(brew --prefix)
    mkdir -p "$HOME/work" && cd "$HOME/work"
    sea-vc --version
    sea-vc --agent > agent.txt
    head -3 agent.txt
    grep -q "^DOCS: $prefix/.*share/doc/sea-mlir (installed" agent.txt
    root=$(sed -n "s/^DOCS: \([^ ]*\) .*/\1/p" agent.txt)
    test -f "$root/docs/agents/index.md"
    for verdict in sat unsat; do
      sea yama -y sea-mlir-export -m64 --horn-sea-mlir-out="$verdict.mlir" \
        "/fixtures/$verdict.c" > "$verdict-export.log" 2>&1 ||
        { cat "$verdict-export.log"; exit 1; }
      grep -q sea.nondet "$verdict.mlir"
      sea-vc "$verdict.mlir" --lower > "$verdict.log"
      cat "$verdict.log"
      grep -q "^result: $verdict\$" "$verdict.log"
      if [ "$verdict" = sat ]; then
        grep -q "^replay: assert-failed" "$verdict.log"
        grep -q "mismatches=0" "$verdict.log"
      fi
    done
    for binary in "$prefix"/bin/sea*; do
      if file -L "$binary" 2> /dev/null | grep -q ELF || head -c4 "$binary" | grep -q ELF; then
        ! ldd "$binary" | grep "not found"
      fi
    done
    echo "smoke: pass"
  ' &
client=$!
wait "$client"
