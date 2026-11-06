#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Query a SeaM rule trail (sea-vc --seam-trail=<file>).

Reads <file> (JSON lines: run / fire / summary records) and, for the term
queries, <file>.terms (the term DAG). Run with --agent for the guide.
"""

import argparse
import json
import sys
from collections import Counter, defaultdict

AGENT = """seam-trail.py agent guide (plain, terse)

WHAT: queries the rule trail sea-vc writes with --seam-trail=<file>: one JSON
record per rule firing (rules, premises, the VC op and its location, the node
ids of lhs and rhs) in <file>, the term DAG in <file>.terms. A firing is
recorded once, when the rule runs; a cache hit is not a firing (counted in the
summary record: cache_hits).

WHY: when a downstream bug shows up (wrong verdict, replay mismatch, solver
error) the trail says which rules produced or touched the terms involved;
the histogram says what a run relied on (which firings use A-INB).

TYPICAL:
  sea-vc p.mlir --lower --seam --seam-trail=t.jsonl
  seam-trail.py t.jsonl                     # histogram per rule / premise / op
  seam-trail.py t.jsonl --premise A-INB     # the rules used under A-INB
  seam-trail.py t.jsonl --rule row.store-skip   # the firings of one rule
  seam-trail.py t.jsonl --contains 1234     # firings whose output contains
                                            #   node 1234 (needs t.jsonl.terms)
  seam-trail.py t.jsonl --produced 1234     # firings whose output is node 1234
  --run R picks the VC run (default: the last; sea-vc runs again without
  A-INB after a static violation).

OUTPUT (stdout, `key: value` lines):
  run: <R>                          the run queried
  firings: <n>                      firings in the run (after --premise)
  cache-hits: <n>  rule-calls: <n>  from the run's summary
  rule.<name>: <n>                  histogram (every rule a firing names)
  premise.<name>: <n>  op.<kind>: <n>
  firing: id=<n> rules=<r,...> premises=<p,...|none> op=<kind> loc=<loc>
      lhs=<id> rhs=<id>             (--rule, --contains, --produced)

STATUS CODES: 0 ok; 1 bad input (missing file, unknown run).
"""


def load(path):
    runs = defaultdict(lambda: {"fire": [], "summary": None})
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            t = r.get("t")
            if t == "fire":
                runs[r["run"]]["fire"].append(r)
            elif t == "summary":
                runs[r["run"]]["summary"] = r
            elif t == "run":
                runs[r["run"]]
    return runs


def load_terms(path, run):
    args = {}
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r["run"] == run:
                args[r["id"]] = r.get("args", [])
    return args


def contains(args, root, target, memo):
    # iterative DFS with memo: does the DAG under root contain target?
    stack = [(root, False)]
    while stack:
        n, done = stack.pop()
        if n in memo:
            continue
        kids = args.get(n, [])
        if n == target:
            memo[n] = True
            continue
        if not done:
            stack.append((n, True))
            stack.extend((k, False) for k in kids if k not in memo)
            continue
        memo[n] = any(memo.get(k, False) for k in kids)
    return memo[root]


def show(r):
    prem = ",".join(r["premises"]) or "none"
    print(
        f"firing: id={r['id']} rules={','.join(r['rules'])} premises={prem} "
        f"op={r['op']} loc={r.get('loc', '-')} lhs={r['lhs']} rhs={r['rhs']}"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trail", nargs="?")
    ap.add_argument("--agent", action="store_true")
    ap.add_argument("--run", type=int)
    ap.add_argument("--premise", help="only firings that use this premise")
    ap.add_argument("--rule", help="list the firings that name this rule")
    ap.add_argument(
        "--contains", type=int, help="firings whose rhs contains this node id"
    )
    ap.add_argument("--produced", type=int, help="firings whose rhs is this node id")
    a = ap.parse_args()
    if a.agent:
        print(AGENT, end="")
        return 0
    if not a.trail:
        ap.error("a trail file is needed")
    try:
        runs = load(a.trail)
    except OSError as e:
        print(f"seam-trail: {e}", file=sys.stderr)
        return 1
    if not runs:
        print("seam-trail: no run in the trail", file=sys.stderr)
        return 1
    run = a.run if a.run is not None else max(runs)
    if run not in runs:
        print(f"seam-trail: no run {run}", file=sys.stderr)
        return 1
    fires = runs[run]["fire"]
    if a.premise:
        fires = [r for r in fires if a.premise in r["premises"]]
    print(f"run: {run}")
    print(f"firings: {len(fires)}")
    s = runs[run]["summary"]
    if s:
        print(f"cache-hits: {s['cache_hits']}")
        print(f"rule-calls: {s['rule_calls']}")
    if a.rule:
        for r in fires:
            if a.rule in r["rules"]:
                show(r)
        return 0
    if a.contains is not None or a.produced is not None:
        if a.produced is not None:
            for r in fires:
                if r["rhs"] == a.produced:
                    show(r)
        if a.contains is not None:
            try:
                args = load_terms(a.trail + ".terms", run)
            except OSError as e:
                print(f"seam-trail: {e}", file=sys.stderr)
                return 1
            memo = {}
            for r in fires:
                if contains(args, r["rhs"], a.contains, memo):
                    show(r)
        return 0
    rules, prems, ops = Counter(), Counter(), Counter()
    for r in fires:
        rules.update(r["rules"])
        prems.update(r["premises"] or ["none"])
        ops[r["op"]] += 1
    for k in sorted(rules):
        print(f"rule.{k}: {rules[k]}")
    for k in sorted(prems):
        print(f"premise.{k}: {prems[k]}")
    for k in sorted(ops):
        print(f"op.{k}: {ops[k]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
