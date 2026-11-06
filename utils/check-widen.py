#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Differential check of sea-widen-memory (byte memory -> aligned k-byte
words).

For each program: O = sccp only, W = sccp + sea-widen-memory{k}. Programs
in which nothing was widened are skipped. O runs with random inputs
(--missing=random, several seeds); every nondet value and memory byte it
reads is replayed into W as an explicit input (the bytes of each k-byte
word combined, little-endian), and W runs with --missing=error. W reads
exactly the words whose bytes O read: the pass widens only memories whose
accesses are all whole, aligned words. O and W must agree on the status
and on every traced value (the pass changes no op, only memory types).
With --sea-vc, the VC must also give the same verdict (and exit status)
with and without --widen-memory=k.

Exit status 1 if any program disagrees (or W asks for an input O never
read: also a disagreement).
"""

import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile


def run(cmd, timeout=120):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return None, "", "timeout"


def field(out, key):
    m = re.search(rf"^{key}: (.*)$", out, re.M)
    return m.group(1) if m else None


def traces(out):
    # O and W are different files: locations of ops without one of their own
    # name the file
    return [
        re.sub(r'\.(O|W)\.mlir"', '.X.mlir"', l)
        for l in out.splitlines()
        if l.startswith("value: ")
    ]


def inputs_from(out, k):
    """--input entries replaying a byte run's reads into the word program."""
    entries = []
    words = collections.defaultdict(dict)  # (site, object) -> {word: {i: v}}
    for line in out.splitlines():
        m = re.match(r"nondet: site=(\d+) .*value=(\d+)", line)
        if m:
            entries.append(f"{m.group(1)}={m.group(2)}")
            continue
        m = re.match(
            r"memread: site=(\d+) addr=(\S+) width=(\d+) "
            r"value=(\d+)",
            line,
        )
        if not m:
            continue
        site, addr, width, value = m.groups()
        if width != "8":
            return None  # not a byte memory read: nothing to replay
        obj, _, off = addr.rpartition("+")
        off = int(off)
        words[(site, obj)].setdefault(off - off % k, {})[off % k] = int(value)
    for (site, obj), ws in words.items():
        for start, parts in ws.items():
            v = sum(b << (8 * i) for i, b in parts.items())
            key = f"{obj}+{start}" if obj else str(start)
            entries.append(f"{site}@{key}={v}")
    return entries


def check(args, path, work):
    name = os.path.basename(path)
    o = os.path.join(work, name + ".O.mlir")
    w = os.path.join(work, name + ".W.mlir")
    rc, _, err = run(
        [
            args.sea_opt,
            path,
            "--pass-pipeline=builtin.module(sea.program(sccp))",
            "-o",
            o,
        ]
    )
    if rc != 0:
        return "skip", f"sccp failed: {err.strip()[:120]}"
    rc, _, err = run(
        [
            args.sea_opt,
            o,
            "--pass-pipeline=builtin.module("
            f"sea.program(sea-widen-memory{{k={args.k} "
            "report=true}))",
            "-o",
            w,
        ]
    )
    if rc != 0:
        return "FAIL", f"widening failed: {err.strip()[:200]}"
    if "widened to" not in err:
        return "skip", "nothing widened"
    for seed in range(args.runs):
        rc, oo, oe = run(
            [args.sea_run, o, "--missing=random", f"--seed={seed}", "--trace"]
        )
        inputs = inputs_from(oo, args.k)
        if inputs is None:
            return "skip", "reads cells wider than a byte"
        cmd = [args.sea_run, w, "--missing=error", "--trace"]
        if inputs:
            cmd.append("--input=" + ",".join(inputs))
        rc2, wo, we = run(cmd)
        so, sw = field(oo, "status"), field(wo, "status")
        if so != sw:
            return "FAIL", (
                f"seed {seed}: byte status {so}, word status {sw}"
                f" ({field(wo, 'message') or we.strip()[:160]})"
            )
        if traces(oo) != traces(wo):
            a, b = traces(oo), traces(wo)
            i = next(
                (i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b))
            )
            return "FAIL", (
                f"seed {seed}: traces differ at {i}: "
                f"{a[i] if i < len(a) else '-'} | "
                f"{b[i] if i < len(b) else '-'}"
            )
    if args.sea_vc:
        # the VC on the byte and on the widened program: same verdict, and
        # the same exit status (2 = a sat that does not replay)
        rb, ob, eb = run([args.sea_vc, path, "--lower"])
        rw, ow, ew = run([args.sea_vc, path, "--lower", f"--widen-memory={args.k}"])
        vb, vw = field(ob, "result"), field(ow, "result")
        if (vb, rb) != (vw, rw) or vb not in ("sat", "unsat"):
            return "FAIL", (
                f"VC: byte {vb} (exit {rb}), word {vw} (exit {rw}) {ew.strip()[:160]}"
            )
    return "ok", None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="+", help=".mlir files or directories")
    ap.add_argument("--sea-opt", required=True)
    ap.add_argument("--sea-run", required=True)
    ap.add_argument(
        "--sea-vc",
        help="also compare sea-vc --lower verdicts with and without --widen-memory=k",
    )
    ap.add_argument("-k", type=int, default=8, help="word size in bytes")
    ap.add_argument("--runs", type=int, default=5, help="random seeds")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    files = []
    for t in args.tests:
        if os.path.isdir(t):
            files += sorted(
                os.path.join(t, f) for f in os.listdir(t) if f.endswith(".mlir")
            )
        else:
            files.append(t)
    with (
        tempfile.TemporaryDirectory() as work,
        concurrent.futures.ThreadPoolExecutor(args.j) as pool,
    ):
        results = list(pool.map(lambda f: check(args, f, work), files))
    counts = collections.Counter(r for r, _ in results)
    for f, (r, why) in zip(files, results):
        if r == "FAIL":
            print(f"FAIL {os.path.basename(f)}: {why}")
    print(
        f"programs: {len(files)} widened-and-checked: {counts['ok']} "
        f"skipped: {counts['skip']} failures: {counts['FAIL']}"
    )
    sys.exit(1 if counts["FAIL"] else 0)


if __name__ == "__main__":
    main()
