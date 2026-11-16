#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""check-cost.py SEA_VC ARGS...: the rlimit counts of --seam-check-cost are
the unit --seam-check-rlimit enforces (test/VC/seam/check-cost.test).

Runs `SEA_VC ARGS --seam-check --seam-check-cost`, takes the firing whose
query has the largest count n, then checks that every firing is decided at
--seam-check-rlimit=2n and that this firing is unknown reason=rlimit at n/10.
Prints `check-cost: ok` or the failure (exit 1)."""

import re
import subprocess
import sys


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout + r.stderr


def main():
    base = sys.argv[1:] + ["--seam-check"]
    out = run(base + ["--seam-check-cost"])
    costs = {}
    for m in re.finditer(
        r"^seam-check-cost: id=(\d+) rlimit=(\d+) time=[0-9.]+$", out, re.M
    ):
        if m.group(1) in costs:
            return fail(f"two cost lines for id={m.group(1)}")
        costs[m.group(1)] = int(m.group(2))
    m = re.search(r"^seam-check: firings=\d+ checked=(\d+) valid=(\d+) ", out, re.M)
    if not m:
        return fail("no seam-check line")
    if len(costs) != int(m.group(1)):
        return fail(f"{len(costs)} cost lines for {m.group(1)} checked firings")
    if int(m.group(2)) != int(m.group(1)):
        return fail("not every firing is valid without a limit")
    top, n = max(costs.items(), key=lambda kv: kv[1])
    if n < 10000:
        return fail(f"the largest count, {n}, is too small to bisect")
    above = run(base + [f"--seam-check-rlimit={2 * n}"])
    if not re.search(r"^seam-check: .* unknown=0 .*unknown-rlimit=0$", above, re.M):
        return fail(f"not every firing is decided at rlimit {2 * n}")
    below = run(base + [f"--seam-check-rlimit={n // 10}"])
    if not re.search(
        rf"^seam-check-unknown: id={top} rules=\S+ reason=rlimit$", below, re.M
    ):
        return fail(f"id={top} (count {n}) is not unknown reason=rlimit at {n // 10}")
    print("check-cost: ok")
    return 0


def fail(why):
    print(f"check-cost: FAIL {why}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
