#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Drive `sea-run --fuzz-serve` from Python (docs/reference/fuzzing.md).

A reference to copy, not a framework. `Server` starts sea-run, sends one
request per run and returns the parsed response. `main()` is one simple
strategy: sample each site from a JSON strategy, keep the inputs that
reach new coverage, stop at the first failure and write it as a corpus
file that `sea-run --input-file` replays.

Strategy (a file, or the JSON text itself):

  {"sites": {"7": {"range": [0, 10]},
             "6": {"range": [0, "s7"]},
             "8": {"choice": [0, 1]}},
   "missing": "random"}

A range is inclusive. A bound is an integer, or another site's value
plus or minus an integer: "s7", "s7 + 1", "s7 - 1"; that site is sampled
first. A bound naming a site the strategy does not list, or a cycle, is
an error. An empty range (lo > hi) discards the whole sample and draws
again (counted as resampled; 1000 in a row is an error). Sites not
listed get `missing` ("zero" or "random"; default "random").
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys


class ServerDied(Exception):
    """sea-run exited before answering (its stderr says why)."""


class Server:
    """One `sea-run <program> --fuzz-serve` process."""

    def __init__(self, program, sea_run="sea-run", args=()):
        self.proc = subprocess.Popen(
            [sea_run, program, "--fuzz-serve", *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def run(self, inputs, **options):
        """Runs once; `inputs` maps "<site>" or "<site>@<addr>" to a value.
        options: missing, seed, trace_sites, id. Returns the response dict
        (kind "response", or "error" for a bad request)."""
        req = {"inputs": {str(k): v for k, v in inputs.items()}, **options}
        try:
            self.proc.stdin.write(json.dumps(req) + "\n")
            self.proc.stdin.flush()
        except BrokenPipeError:
            self.died()
        line = self.proc.stdout.readline()
        if not line:
            self.died()
        return json.loads(line)

    def close(self):
        """Ends the server; returns its summary record."""
        try:
            self.proc.stdin.close()
        except BrokenPipeError:
            pass
        last = None
        for line in self.proc.stdout:
            last = json.loads(line)
        if self.proc.wait() != 0 or last is None or last["kind"] != "summary":
            self.died()
        return last

    def died(self):
        code = self.proc.wait()
        raise ServerDied(f"sea-run --fuzz-serve exited with code {code}")


BOUND = re.compile(r"s([0-9]+)\s*(?:([+-])\s*([0-9]+))?", re.ASCII)


def parse_bound(b):
    """An integer, or "s<N>", "s<N> + k", "s<N> - k": (site, offset)."""
    if isinstance(b, int) and not isinstance(b, bool):
        return (None, b)
    m = BOUND.fullmatch(b.strip()) if isinstance(b, str) else None
    if not m:
        sys.exit(f"bad bound {b!r}: an integer, s<N>, s<N> + k or s<N> - k")
    k = int(m[3] or 0)
    return (m[1], -k if m[2] == "-" else k)


def check_spec(site, spec):
    """A site spec is {"range": [lo, hi]} or {"choice": [v, ...]}."""
    ok = isinstance(spec, dict) and (
        (
            "range" in spec
            and isinstance(spec["range"], list)
            and len(spec["range"]) == 2
        )
        or ("choice" in spec and isinstance(spec["choice"], list) and spec["choice"])
    )
    if not ok:
        sys.exit(
            f"bad spec for site {site}: "
            '{"range": [lo, hi]} or {"choice": [v, ...]} (non-empty)'
        )


def plan(sites):
    """The sites in sampling order (a site after the sites its bounds
    name), with parsed bounds. A bound naming a site the strategy does not
    list, or a cycle, is an error."""
    if not isinstance(sites, dict):
        sys.exit('bad strategy: "sites" must be an object of site specs')
    order, state = [], {}

    def visit(site):
        if state.get(site) == "done":
            return
        if site not in sites:
            sys.exit(f"a bound names site {site}, which the strategy does not list")
        if state.get(site) == "open":
            sys.exit(f"the bounds of site {site} depend on itself")
        state[site] = "open"
        spec = sites[site]
        check_spec(site, spec)
        if "range" in spec:
            spec["bounds"] = [parse_bound(b) for b in spec["range"]]
            for ref, _ in spec["bounds"]:
                if ref is not None:
                    visit(ref)
        state[site] = "done"
        order.append(site)

    for site in sites:
        visit(site)
    return order


def sample(sites, order, rng):
    """One input, or None when a range is empty (lo > hi): resample."""
    values = {}
    for site in order:
        spec = sites[site]
        if "choice" in spec:
            values[site] = rng.choice(spec["choice"])
            continue
        lo, hi = (k if ref is None else values[ref] + k for ref, k in spec["bounds"])
        if lo > hi:
            return None
        values[site] = rng.randint(lo, hi)
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("program")
    ap.add_argument(
        "--strategy", required=True, help="a JSON strategy file, or the JSON text"
    )
    ap.add_argument("--requests", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument(
        "--out", default="failure.txt", help="the corpus file of the first failure"
    )
    ap.add_argument("--sea-run", default="sea-run")
    ap.add_argument(
        "--sea-run-arg",
        action="append",
        default=[],
        help="passed to sea-run (e.g. --defer-memory-errors)",
    )
    a = ap.parse_args()
    text = a.strategy
    if os.path.exists(text):
        with open(text) as f:
            text = f.read()
    strategy = json.loads(text)
    if not isinstance(strategy, dict):
        sys.exit('bad strategy: a JSON object with "sites"')
    sites = strategy.get("sites", {})
    order = plan(sites)
    missing = strategy.get("missing", "random")

    try:
        return drive(a, sites, order, missing)
    except ServerDied as e:
        sys.exit(f"fuzz-driver: {e}")


def drive(a, sites, order, missing):
    """The loop of main(): sample, run, keep, stop at a failure."""
    rng = random.Random(a.seed)
    server = Server(a.program, a.sea_run, a.sea_run_arg)
    kept, blocked, status, found, resampled = [], {}, {}, None, 0
    n = 0
    for i in range(a.requests):
        n = i + 1
        for _ in range(1000):
            inputs = sample(sites, order, rng)
            if inputs is not None:
                break
            resampled += 1
        else:
            sys.exit("1000 samples in a row had an empty range")
        r = server.run(inputs, missing=missing, seed=i)
        if r["kind"] == "error":
            sys.exit("bad request: " + r["error"])
        status[r["status"]] = status.get(r["status"], 0) + 1
        if any(r["new"].values()):
            kept.append(r["input"])
        if "blocked" in r:
            key = (r["blocked"], r["blocked_at"])
            blocked[key] = blocked.get(key, 0) + 1
        if r["status"] in ("assert-failed", "memory-error"):
            found = r
            break
    summary = server.close()

    print(f"requests: {n} kept={len(kept)} resampled={resampled}")
    print("status: " + " ".join(f"{k}={v}" for k, v in status.items()))
    print("coverage: " + " ".join(f"{k}={v}" for k, v in summary["coverage"].items()))
    for (aid, at), count in sorted(blocked.items(), key=lambda x: -x[1]):
        print(f"blocked: {count} assume={aid} at={at}")
    if found:
        with open(a.out, "w") as f:
            f.write(f"# fuzz-driver: request {n} status={found['status']}\n")
            f.write("\n".join(found["input"]) + "\n")
        print(f"failure: {found['status']} file={a.out}")
        print(found["text"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
