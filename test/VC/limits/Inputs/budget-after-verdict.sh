#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# usage: budget-after-verdict.sh <sea-vc> <program.mlir>
# Runs sea-vc with a --max-nodes budget that only the replay (after the
# solver's verdict) exceeds: the limit is the middle between the most live
# nodes at the end of the phases up to the solve and the live nodes at the
# end of the replay, both read from the same program's --mem-trace
# phase-end lines (nodes are deterministic, so the limit follows the
# encoder instead of pinning a count). Prints sea-vc's stdout; exits with
# its status (0 for the verdict sat); stderr is kept.
set -e
sv=$1
prog=$2
trace=$("$sv" "$prog" --lower --seam --mem-trace --mem-trace-interval=0 \
  2>&1 >/dev/null | grep 'vc.mem phase-end')
live() { tr ' ' '\n' | grep '^nodes-live=' | cut -d= -f2; }
pre=$(echo "$trace" | grep -v 'phase=replay ' | live | sort -n | tail -1)
post=$(echo "$trace" | grep 'phase=replay ' | live)
if [ $((post - pre)) -lt 8 ]; then
  echo "no room for a node budget between $pre and $post" >&2
  exit 1
fi
exec "$sv" "$prog" --lower --seam --max-nodes=$(((pre + post) / 2))
