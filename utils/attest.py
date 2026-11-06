#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""attest: run the gates CI cannot run and post them as commit statuses.

    utils/attest.py [--only GATE[,GATE...]] [--dry-run] [--build DIR]
                    [--base REF] [--base-sea-vc PATH] [--perf-runs N]
                    [--perf-threshold PCT] [--repo OWNER/NAME]
                    [-- CMAKE_ARG...]
    utils/attest.py --agent | --self-test

`utils/attest.py --agent` prints the guide.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_HOME = os.path.abspath(
    os.path.expanduser(
        os.environ.get(
            "SEA_BENCH_HOME",
            os.path.join(
                os.environ.get("XDG_CACHE_HOME", "~/.cache"), "sea-mlir-bench"
            ),
        )
    )
)
GATES = ["check-sea", "bench-vcc", "bench-perf"]
CONTEXT = "sea-mlir/{}"
VCC_SUITE = "public/vcc"
VCC_TIER = "quick"
DESC_MAX = 140

AGENT = """attest.py agent guide (plain, terse)
WHAT: run the gates GitHub Actions cannot run (they need LLVM, the solvers and the
  benchmark suites) on the current HEAD and post each result as a GitHub commit status
  on that exact SHA. The ruleset of branch main requires sea-mlir/check-sea and
  sea-mlir/bench-vcc (docs/agents/gates.md).
WHEN: after the last commit of a pull request is pushed; a new push needs a new attest
  (statuses belong to a SHA). Refuses a dirty tree (untracked files included).
GATES (contexts):
  sea-mlir/check-sea   ninja -C BUILD, then ninja -C BUILD check-sea (LIT_OPTS default -j4 -s);
                       description: `N/M passed[, K unsupported]`.
  sea-mlir/bench-vcc   bench.py gate quick --suite public/vcc --par 4 --no-record --sea-vc BUILD/bin/sea-vc
                       (needs `just bench-connect`); description: jobs, failures, counter failures, wall.
  sea-mlir/bench-perf  per-job median CPU time of the vcc gate (default config: z3) vs the base commit;
                       fails when the total over jobs both sides solve grows by more than
                       --perf-threshold percent (default 10). Yices timings need several seeds
                       per job and are not compared.
RECORDS: $SEA_BENCH_HOME/attest/<sha>.json: the results dirs of every vcc gate run at <sha>
  (sea-vc sha256 included). The base of bench-perf is --base (default: merge-base of HEAD
  and origin/main, or HEAD~1 on main itself); without a record for it, pass
  --base-sea-vc PATH (a sea-vc built at the base): attest runs base and HEAD interleaved
  and records both.
OPTIONS:
  --only check-sea,bench-vcc,bench-perf   (default: all three)
  --build DIR         build directory (default: build-attest); configured with
                      -G Ninja -DCMAKE_BUILD_TYPE=Release and the CMAKE_ARGs after `--`
                      if it has no CMakeCache.txt; it must be a build of this source tree
  --dry-run           run the gates, print the statuses instead of posting them
  --perf-runs N       vcc gate runs per side for bench-perf (default 3; medians)
  --repo OWNER/NAME   default: from the origin remote
OUTPUT: `key: value` lines; one `status: <context> <state> <description>` per gate, then
  `posted:` or `would-post:` lines (statuses are posted at the end, after the tree is
  checked again; a build failure is a check-sea failure). EXIT: 0 every gate passed, 1 a gate failed,
  2 usage / dirty tree / HEAD moved, 3 a gate could not run (no status posted for it).
"""


class Refuse(Exception):
    """A precondition failed: exit 2, nothing posted."""


class Skip(Exception):
    """A gate could not run: no status for it, exit 3."""


def run(cmd, **kw):
    return subprocess.run(cmd, text=True, **kw)


def git(*args):
    return run(
        ["git", "-C", REPO, *args], capture_output=True, check=True
    ).stdout.strip()


def tree_state():
    """(sha, dirty): dirty includes untracked, non-ignored files."""
    return git("rev-parse", "HEAD"), bool(git("status", "--porcelain"))


def repo_slug(url):
    """OWNER/NAME of a GitHub remote URL (https or ssh form)."""
    m = re.search(r"github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?/?$", url)
    if not m:
        raise Refuse(f"origin is not a GitHub URL: {url} (--repo)")
    return f"{m.group(1)}/{m.group(2)}"


def clip(text):
    return text if len(text) <= DESC_MAX else text[: DESC_MAX - 3] + "..."


def fmt_secs(s):
    s = int(round(s))
    return f"{s // 60}m{s % 60:02d}s" if s >= 60 else f"{s}s"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- check-sea
LIT_COUNT = re.compile(r"^\s*([A-Z][A-Za-z ]*?)\s*:\s*(\d+)\b", re.M)


def parse_lit(text):
    """{'total': N, 'Passed': N, 'Failed': N, ...} from lit's summary."""
    out = {}
    m = re.search(r"Total Discovered Tests:\s*(\d+)", text)
    if m:
        out["total"] = int(m.group(1))
        for k, v in LIT_COUNT.findall(text[m.end() :]):
            out[k] = int(v)
    return out


def lit_description(counts, ok, secs):
    total = counts.get("total")
    if total is None:
        return "lit summary not found" + ("" if ok else "; check-sea failed")
    passed = counts.get("Passed", 0) + counts.get("Expectedly Failed", 0)
    ran = total - counts.get("Unsupported", 0) - counts.get("Excluded", 0)
    desc = f"{passed}/{ran} passed"
    bad = {
        k: v
        for k, v in counts.items()
        if k in ("Failed", "Unexpectedly Passed", "Timed Out", "Unresolved") and v
    }
    desc += "".join(f", {v} {k.lower()}" for k, v in bad.items())
    if counts.get("Unsupported"):
        desc += f", {counts['Unsupported']} unsupported"
    return desc + f" ({fmt_secs(secs)})"


def ensure_build(a):
    build = os.path.abspath(a.build)
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.exists(cache):
        cmd = ["cmake", "-S", REPO, "-B", build, "-G", "Ninja"]
        cmd += ["-DCMAKE_BUILD_TYPE=Release", *a.cmake_args]
        print(f"configure: {shlex.join(cmd)}", flush=True)
        if run(cmd).returncode:
            raise Refuse(f"cmake failed in {build}")
    else:
        src = re.search(
            r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", open(cache).read(), re.M
        )
        if not src or os.path.realpath(src.group(1)) != os.path.realpath(REPO):
            raise Refuse(f"{build} is not a build of {REPO}")
    print(f"build: {build}", flush=True)
    if run(["ninja", "-C", build]).returncode:
        print(f"build-failed: {build}")
        return None
    return build


def gate_check_sea(a, build):
    env = dict(os.environ, LIT_OPTS=os.environ.get("LIT_OPTS", "-j4") + " -s")
    t0 = time.monotonic()
    p = run(["ninja", "-C", build, "check-sea"], env=env, capture_output=True)
    secs = time.monotonic() - t0
    sys.stdout.write(p.stdout[-4000:])
    ok = p.returncode == 0
    return ("success" if ok else "failure"), lit_description(
        parse_lit(p.stdout), ok, secs
    )


# ---------------------------------------------------------------- bench gates
def parse_gate(text):
    """The `key: value` lines of `bench.py gate` (last value wins); the first
    `gate:` line (tier, configs, jobs) is kept as `header`."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"^([a-z-]+): (.*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
            if m.group(1) == "gate":
                out.setdefault("header", m.group(2))
    return out


def gate_description(g):
    m = re.search(r"jobs=(\d+)", g.get("header", ""))
    jobs = m.group(1) if m else "?"
    return (
        f"{jobs} jobs, {g.get('failures', '?')} failures, "
        f"{g.get('counter-failures', '?')} counter failures, "
        f"wall {fmt_secs(float(g.get('wall', 0)))} ({VCC_TIER} {VCC_SUITE})"
    )


def run_vcc_gate(sea_vc, par):
    cmd = [sys.executable, os.path.join(REPO, "utils", "bench.py"), "gate", VCC_TIER]
    cmd += ["--suite", VCC_SUITE, "--par", str(par), "--no-record", "--sea-vc", sea_vc]
    print(f"run: {shlex.join(cmd)}", flush=True)
    p = run(cmd, capture_output=True)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr[-2000:])
    g = parse_gate(p.stdout)
    if "out" not in g or "verdicts" not in g:
        raise Skip(
            f"bench.py gate did not run (rc {p.returncode}; `just bench-connect`?)"
        )
    g["pass"] = p.returncode == 0 and g.get("gate") == "pass"
    return g


def record_path(sha):
    return os.path.join(BENCH_HOME, "attest", f"{sha}.json")


def load_record(sha):
    try:
        with open(record_path(sha)) as f:
            return json.load(f)
    except FileNotFoundError:
        return {"sha": sha, "vcc": []}


def add_record(sha, sea_vc, g, supplied=False):
    rec = load_record(sha)
    rec["vcc"].append(
        {
            "date": datetime.datetime.now().isoformat(timespec="seconds"),
            "results": g["out"],
            "pass": g["pass"],
            "sea-vc": sea_vc,
            "sea-vc-sha256": sha256(sea_vc),
            "supplied-binary": supplied,
        }
    )
    os.makedirs(os.path.dirname(record_path(sha)), exist_ok=True)
    tmp = record_path(sha) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(rec, f, indent=1)
    os.replace(tmp, record_path(sha))


def load_times(results_dirs, metric):
    """{job: [time per run]} of the default configuration, solved runs only."""
    per = {}
    for d in results_dirs:
        with open(os.path.join(d, "rows.jsonl")) as f:
            for line in f:
                r = json.loads(line)
                if r.get("config") == "default" and r.get("verdict") in (
                    "sat",
                    "unsat",
                ):
                    per.setdefault(r["job"], []).append(float(r[metric]))
    return per


def compare_perf(base, head, threshold, min_time=1.0):
    """Median per job over the jobs both sides solved. Returns a dict with the
    totals, the ratio, and the jobs slower by more than THRESHOLD percent and
    more than MIN_TIME seconds."""
    common = sorted(set(base) & set(head))
    mb = {j: statistics.median(base[j]) for j in common}
    mh = {j: statistics.median(head[j]) for j in common}
    tb, th = sum(mb.values()), sum(mh.values())
    limit = 1 + threshold / 100
    slower = [
        (j, mb[j], mh[j])
        for j in common
        if mh[j] > mb[j] * limit and mh[j] - mb[j] > min_time
    ]
    slower.sort(key=lambda x: x[1] - x[2])
    return {
        "jobs": len(common),
        "base": tb,
        "head": th,
        "ratio": th / tb if tb else 1.0,
        "slower": slower,
        "pass": not tb or th <= tb * limit,
    }


def perf_description(c, threshold, base_sha, runs):
    pct = (c["ratio"] - 1) * 100
    return (
        f"z3 cpu {c['head']:.1f}s vs {c['base']:.1f}s ({pct:+.1f}%, limit "
        f"+{threshold:g}%) over {c['jobs']} jobs; {len(c['slower'])} jobs slower; "
        f"base {base_sha[:10]}, {runs} runs"
    )


def default_base(sha):
    try:
        upstream = git("rev-parse", "origin/main")
    except subprocess.CalledProcessError as e:
        raise Refuse("no origin/main: pass --base REF") from e
    base = git("merge-base", sha, upstream)
    return git("rev-parse", f"{sha}~1") if base == sha else base


# ---------------------------------------------------------------- main
def post(a, sha, context, state, desc):
    desc = clip(desc)
    line = f"{context} {state} {desc}"
    if a.dry_run:
        print(f"would-post: {a.repo} {sha} {line}")
        return
    cmd = ["gh", "api", "-X", "POST", f"repos/{a.repo}/statuses/{sha}"]
    cmd += [
        "-f",
        f"state={state}",
        "-f",
        f"context={context}",
        "-f",
        f"description={desc}",
    ]
    p = run(cmd, capture_output=True)
    if p.returncode:
        raise Refuse(f"gh api failed: {p.stderr.strip()}")
    print(f"posted: {a.repo} {sha} {line}")


def attest(a):
    sha, dirty = tree_state()
    if dirty:
        raise Refuse("the tree is dirty (commit, stash or remove untracked files)")
    a.repo = a.repo or repo_slug(git("remote", "get-url", "origin"))
    only = [g for g in (a.only.split(",") if a.only else GATES) if g]
    bad = [g for g in only if g not in GATES]
    if bad:
        raise Refuse(f"unknown gate {bad} (gates: {','.join(GATES)})")
    print(f"sha: {sha}")
    print(f"repo: {a.repo}")
    print(f"gates: {','.join(only)}", flush=True)

    build = ensure_build(a)
    sea_vc = os.path.join(build or "", "bin", "sea-vc")
    results = {}
    head_gate = None
    for gate in only:
        try:
            if not build:
                if gate != "check-sea":
                    raise Skip("the build failed")
                results[gate] = ("failure", "the build failed")
            elif gate == "check-sea":
                results[gate] = gate_check_sea(a, build)
            elif gate == "bench-vcc":
                head_gate = run_vcc_gate(sea_vc, a.par)
                add_record(sha, sea_vc, head_gate)
                results[gate] = (
                    "success" if head_gate["pass"] else "failure",
                    gate_description(head_gate),
                )
            else:
                results[gate] = gate_bench_perf(a, sha, sea_vc)
        except Skip as e:
            print(f"skip: {gate}: {e}")
            results[gate] = None
        st = results[gate]
        print(
            f"status: {CONTEXT.format(gate)} " + (f"{st[0]} {st[1]}" if st else "none")
        )

    if tree_state() != (sha, False):
        raise Refuse(
            "HEAD moved or the tree changed while the gates ran: nothing posted"
        )
    for gate in only:
        if results[gate]:
            post(a, sha, CONTEXT.format(gate), *results[gate])
        else:
            post(
                a, sha, CONTEXT.format(gate), "error", "could not run (utils/attest.py)"
            )
    if any(r is None for r in results.values()):
        return 3
    return 0 if all(r[0] == "success" for r in results.values()) else 1


def gate_bench_perf(a, sha, sea_vc):
    base = git("rev-parse", a.base) if a.base else default_base(sha)
    print(f"perf-base: {base}", flush=True)
    base_dirs = [r["results"] for r in load_record(base)["vcc"]]
    head_dirs = [r["results"] for r in load_record(sha)["vcc"]]
    interleave = not base_dirs
    if interleave and not a.base_sea_vc:
        raise Skip(f"no recorded vcc gate at the base {base[:12]}: --base-sea-vc PATH")
    for i in range(a.perf_runs):
        if interleave:
            g = run_vcc_gate(os.path.abspath(a.base_sea_vc), a.par)
            add_record(base, os.path.abspath(a.base_sea_vc), g, supplied=True)
            base_dirs.append(g["out"])
        if len(head_dirs) <= i:
            g = run_vcc_gate(sea_vc, a.par)
            add_record(sha, sea_vc, g)
            head_dirs.append(g["out"])
    runs = min(len(base_dirs), len(head_dirs))
    c = compare_perf(
        load_times(base_dirs, "cpu"), load_times(head_dirs, "cpu"), a.perf_threshold
    )
    print(
        f"perf-total: base={c['base']:.2f} head={c['head']:.2f} ratio={c['ratio']:.3f}"
    )
    for j, b, h in c["slower"][:10]:
        print(f"perf-slower: {j} base={b:.2f} head={h:.2f} ratio={h / b:.2f}")
    state = "success" if c["pass"] else "failure"
    return state, perf_description(c, a.perf_threshold, base, runs)


# ---------------------------------------------------------------- self-test
def self_test():
    lit = """Total Discovered Tests: 398
  Unsupported: 2 (0.50%)
  Passed     : 395 (99.25%)
  Failed     : 1 (0.25%)
"""
    c = parse_lit(lit)
    assert c == {"total": 398, "Unsupported": 2, "Passed": 395, "Failed": 1}, c
    d = lit_description(c, False, 125)
    assert d == "395/396 passed, 1 failed, 2 unsupported (2m05s)", d
    assert lit_description(parse_lit(""), True, 1) == "lit summary not found"
    g = parse_gate(
        "gate: quick configs=default jobs=228 runs=228 par=4\nout: /r/x\n"
        "verdicts: sat=10 unsat=218\nfailures: 0\ncounter-failures: 0\n"
        "wall: 61\ngate: pass\n"
    )
    assert g["out"] == "/r/x" and g["gate"] == "pass"
    d = gate_description(g)
    assert d.startswith("228 jobs, 0 failures, 0 counter failures, wall 1m01s"), d
    base = {"a": [1.0, 2.0, 3.0], "b": [10.0], "c": [5.0]}
    head = {"a": [2.0, 2.0, 9.0], "b": [13.0], "d": [1.0]}
    c = compare_perf(base, head, 10)
    assert c["jobs"] == 2 and c["base"] == 12 and c["head"] == 15, c
    assert not c["pass"] and [j for j, _, _ in c["slower"]] == ["b"], c
    assert compare_perf(base, base, 10)["pass"]
    assert len(clip("x" * 200)) == DESC_MAX
    assert repo_slug("https://github.com/seahorn/sea-mlir.git") == "seahorn/sea-mlir"
    assert repo_slug("git@github.com:seahorn/sea-mlir") == "seahorn/sea-mlir"
    with tempfile.TemporaryDirectory() as d:
        rows = [
            {"job": "x", "config": "default", "verdict": "unsat", "cpu": 1.5},
            {"job": "y", "config": "default", "verdict": "timeout", "cpu": 9},
            {"job": "x", "config": "default+yices", "verdict": "unsat", "cpu": 7},
        ]
        with open(os.path.join(d, "rows.jsonl"), "w") as f:
            f.write("".join(json.dumps(r) + "\n" for r in rows))
        assert load_times([d], "cpu") == {"x": [1.5]}
    print("self-test: pass")
    return 0


def main():
    argv = sys.argv[1:]
    cmake_args = []
    if "--" in argv:
        i = argv.index("--")
        argv, cmake_args = argv[:i], argv[i + 1 :]
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--agent", action="store_true", help="print the guide")
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--only", help=f"comma-separated gates ({','.join(GATES)})")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--build", default=os.path.join(REPO, "build-attest"))
    p.add_argument("--base", help="base commit of bench-perf")
    p.add_argument("--base-sea-vc", help="a sea-vc built at the base")
    p.add_argument("--perf-runs", type=int, default=3)
    p.add_argument("--perf-threshold", type=float, default=10.0, help="percent")
    p.add_argument("--par", type=int, default=4)
    p.add_argument("--repo", help="OWNER/NAME (default: the origin remote)")
    a = p.parse_args(argv)
    a.cmake_args = cmake_args
    if a.agent:
        print(AGENT, end="")
        return 0
    if a.self_test:
        return self_test()
    try:
        return attest(a)
    except Refuse as e:
        print(f"attest: refused: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
