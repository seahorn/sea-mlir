#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Driver of the test/VC/memops corpus (disposable tests, not oracles).

Directives, in comments of the .mlir file:
  // EXPECT: <program> sat|unsat|error:<code> [warn:<code>] [-- <extra sea-vc flags>]
      sea-vc --lower [flags] --program=<program>, with z3 and Yices (when
      sea-vc is built with it: `--version`), plain and
      --seam: the same verdict every time. sat: replay: assert-failed and
      validate mismatches=0. error:<code>: exit status != 0 and <code> in the
      output. warn:<code>: <code> in the output.
  // EXPECT-SEAM: <program> ... (same syntax)
      overrides EXPECT for the --seam runs of <program> (plain keeps EXPECT):
      --seam does not run sea-expand-memset (docs/design/SeaM-memops.md), so
      its symbolic lengths are exact (no n <= K bound, no
      memset.symbolic-bound warning; the intended domain change).
  // INTERP: <program> assert-failed|halted <site[@obj+off]=value,...>
      sea-run --missing=zero --input=...: the status line (the verdict the
      interpreter gives on an input chosen by hand, independent of sea-vc).
      a spec <site>@<obj>+*=<lo>+<step> sets cells 0..63 of the object to
      lo + step*i (cells past the object's size are never read).
"""

import os
import subprocess
import sys


def main():
    path, vc = sys.argv[1], sys.argv[2]
    run = os.path.join(os.path.dirname(vc), "sea-run")
    version = subprocess.run([vc, "--version"], capture_output=True, text=True).stdout
    solvers = ("z3",) if "yices: not built" in version else ("z3", "yices")
    fails = 0
    seam_override = {}
    for line in open(path):
        line = line.strip()
        if line.startswith("// EXPECT-SEAM:"):
            body = line[len("// EXPECT-SEAM:") :].strip()
            seam_override[body.split()[0]] = body
    for line in open(path):
        line = line.strip()
        if line.startswith("// EXPECT:"):
            plain = line[len("// EXPECT:") :].strip()
            prog0 = plain.split()[0]
            for seam in ([], ["--seam"]):
                body = seam_override.get(prog0, plain) if seam else plain
                extra = []
                if " -- " in body:
                    body, e = body.split(" -- ")
                    extra = e.split()
                toks = body.split()
                prog, verdict = toks[0], toks[1]
                warns = [t[5:] for t in toks[2:] if t.startswith("warn:")]
                for solver in solvers:
                    if True:
                        cmd = (
                            [
                                vc,
                                path,
                                "--lower",
                                "--program=" + prog,
                                "--solver=" + solver,
                            ]
                            + seam
                            + extra
                        )
                        r = subprocess.run(cmd, capture_output=True, text=True)
                        out = r.stdout + r.stderr
                        tag = f"{prog} {solver} {' '.join(seam) or 'plain'}"
                        ok = True
                        if verdict.startswith("error:"):
                            ok = r.returncode != 0 and verdict[6:] in out
                        else:
                            ok = r.returncode == 0 and f"result: {verdict}" in out
                            if ok and verdict == "sat":
                                ok = (
                                    "replay: assert-failed" in out
                                    and "mismatches=0" in out
                                )
                        for w in warns:
                            ok = ok and w in out
                        if not ok:
                            fails += 1
                            print(f"FAIL {tag}: expected {verdict}\n{out}")
        elif line.startswith("// INTERP:"):
            toks = line.split()[2:]
            prog, status = toks[0], toks[1]
            ins = []
            for spec in toks[2:]:
                key, val = spec.split("=")
                if key.endswith("+*"):  # <site>@<obj>+*=<lo>+<step>: 64 cells
                    lo, step = val.split("+")
                    ins += [
                        f"{key[:-1]}{i}={int(lo) + int(step) * i}" for i in range(64)
                    ]
                else:
                    ins.append(spec)
            inputs = ",".join(ins)
            cmd = [run, path, "--program=" + prog, "--missing=zero"]
            if inputs and inputs != "-":
                cmd.append("--input=" + inputs)
            r = subprocess.run(cmd, capture_output=True, text=True)
            if f"status: {status}" not in r.stdout:
                fails += 1
                print(f"FAIL sea-run {prog}: expected {status}\n{r.stdout}{r.stderr}")
    sys.exit(1 if fails else 0)


main()
