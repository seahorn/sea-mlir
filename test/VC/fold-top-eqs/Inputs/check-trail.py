# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Reads a --model-trail-out file back: every line one JSON object with the
documented keys, the counts per kind equal the vc.model-trail counters of the
same run, no variable twice, and (with --z3) every term parses as SMT-LIB
against the declarations of the --smt2 query (eliminated variables are not
declared there and must not occur in a term)."""

import argparse
import json
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trail")
    ap.add_argument("stats")
    ap.add_argument("--smt2")
    ap.add_argument("--z3")
    a = ap.parse_args()
    entries = [json.loads(line) for line in open(a.trail) if line.strip()]
    m = re.search(
        r"^vc\.model-trail: cells=(\d+) defs=(\d+)", open(a.stats).read(), re.M
    )
    if not m:
        sys.exit("no vc.model-trail line")
    want = {"cell": int(m.group(1)), "def": int(m.group(2))}
    got = {"cell": 0, "def": 0}
    seen = set()
    for e in entries:
        kind = e["kind"]
        keys = {"cell": {"kind", "var", "array", "index", "value"}}.get(
            kind, {"kind", "var", "value"}
        )
        if kind not in got or set(e) != keys:
            sys.exit("bad entry: %s" % e)
        if e["var"] in seen:
            sys.exit("variable twice: %s" % e["var"])
        seen.add(e["var"])
        got[kind] += 1
    if got != want:
        sys.exit("counts %s, counters %s" % (got, want))
    if a.z3:
        decls = [
            line
            for line in open(a.smt2)
            if line.startswith("(declare-fun") or line.startswith("(declare-const")
        ]
        body = []
        for e in entries:
            for k in ("index", "value"):
                if k in e:
                    body.append("(assert (= %s %s))" % (e[k], e[k]))
        r = subprocess.run(
            [a.z3, "-in"],
            input="".join(decls) + "\n".join(body) + "\n(check-sat)\n",
            capture_output=True,
            text=True,
        )
        if r.stdout.strip() != "sat":
            sys.exit("z3 does not parse the terms: %s" % (r.stdout + r.stderr))
    print("trail: entries=%d cells=%d defs=%d ok" % (len(entries), *got.values()))


if __name__ == "__main__":
    main()
