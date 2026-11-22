#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# usage: budget-after-reach.sh <sea-vc> <program.mlir>
# Runs sea-vc --reach with a --max-rss budget that only the check at the
# reach phase's end (after runReach returned: the targets are decided)
# exceeds. The size at that end is read from the same program's --mem-trace
# phase-end line (the solver's memory shows only in the reach phase, a few
# MiB above its start); the limit is 2 MiB below it. --max-nodes cannot
# tell here: the live nodes at the end of the phase are fewer than before.
# The process size is measured, so the script fails when the end is not
# more than 5 MiB above the phases before the solver (no solver memory
# seen). Prints sea-vc's stdout; exits with its status.
set -e
sv=$1
prog=$2
trace=$("$sv" "$prog" --lower --reach --mem-trace --mem-trace-interval=0 \
  2>&1 >/dev/null | grep 'vc.mem phase-end')
rss() { tr ' ' '\n' | grep '^rss-mb=' | cut -d= -f2; }
pre=$(echo "$trace" | grep -v 'phase-end phase=reach ' | rss | sort -n | tail -1)
post=$(echo "$trace" | grep 'phase-end phase=reach ' | rss)
if [ $((post - pre)) -lt 5 ]; then
  echo "no solver memory seen: $pre then $post MiB" >&2
  exit 1
fi
exec "$sv" "$prog" --lower --reach --max-rss=$((post - 2))
