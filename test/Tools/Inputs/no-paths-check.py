#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""No binary holds a given path (what `strings <bin> | grep <path>` finds).

usage: no-paths-check.py --path P [--path P ...] BINARY...

Prints `no-paths.found: <binary>: <path> x<n>` per binary and path that
occur, and `no-paths.checked: <n> binaries`; exit 1 if any occurs.
"""

import argparse
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", action="append", required=True)
    ap.add_argument("binaries", nargs="+")
    a = ap.parse_args()
    found = 0
    for b in a.binaries:
        with open(b, "rb") as f:
            data = f.read()
        for p in a.path:
            n = data.count(p.encode())
            if n:
                found += 1
                print(f"no-paths.found: {b}: {p} x{n}")
    print(f"no-paths.checked: {len(a.binaries)} binaries")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
