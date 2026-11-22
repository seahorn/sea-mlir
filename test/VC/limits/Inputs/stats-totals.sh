#!/bin/sh
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# usage: stats-totals.sh <sea-vc> <program.mlir> <from> <step> <to>
# The budget stop's seam.rule-calls over a growing --max-nodes: never 0 once
# rules have run (a stop inside a nested rewrite, which counts on a scratch
# set, must show the totals, not the scratch set's zeros), and equal to the
# rule-calls of the `budget: hit` line. (It may drop a little between two
# limits: a nested rewrite's unresolved counts are dropped when it ends.)
# Prints `totals` or the first violation.
sv=$1
prog=$2
n=$3
while [ "$n" -le "$5" ]; do
  out=$("$sv" "$prog" --lower --seam --max-nodes="$n" --seam-stats 2>/dev/null)
  c=$(echo "$out" | grep '^seam.rule-calls:' | cut -d' ' -f2)
  b=$(echo "$out" | grep '^budget: hit' | tr ' ' '\n' | grep '^rule-calls=' | cut -d= -f2)
  if [ -z "$c" ] || [ "$c" != "$b" ] || [ "$c" -eq 0 ]; then
    echo "violation at --max-nodes=$n: seam.rule-calls=$c budget=$b"
    exit 0
  fi
  n=$((n + $4))
done
echo totals
