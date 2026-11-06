#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""reduce-test: interestingness tests for sea-reduce, and the reduction loop.

sea-reduce (mlir-reduce) keeps a candidate when an external test exits
nonzero (mlir/lib/Reducer/Tester.cpp). Writing that test by hand is where
reductions go wrong: a parse error, a BMC-form error or an unrelated crash
"keeps" the symptom and the result answers the wrong question. This
script is that test for the usual questions, with the failure modes made
uninteresting:

  crash            a tool (sea-vc by default) dies by a signal and/or
                   prints a given diagnostic (regex)
  verdict-differs  two sea-vc configurations both answer sat / unsat and
                   the answers differ
  slow             configuration A exceeds a budget (time, memory, or a
                   --seam-stats counter) while configuration B stays under
                   a smaller one; budgets enforced (timeout + RLIMIT_AS)
  result           a configuration gives a given result (and status)

Every mode first runs `sea-opt <file>` (parse + verify): a candidate that
fails is uninteresting. Outside crash mode, a sea-vc run that reports an
error (status 1) or dies is uninteresting too.

Subcommands:
  test   <mode options> <file>      exit 1 interesting, 0 not
  script <mode options> -o t.sh     write a test script for sea-reduce
  reduce <mode options> in.mlir -o out.mlir
                                    check the input is interesting, then
                                    run sea-reduce rounds to a fixpoint;
                                    print the outcome table per stage
  summary <log>                     the outcome table of a --log file
  audit <mode options> in.mlir      each pattern once at sampled sites
"""

import argparse
import hashlib
import json
import os
import re
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

AGENT = """\
reduce-test: interestingness tests for sea-reduce, and the reduction loop.
WHY: a hand-written test lets parse errors, BMC-form errors or unrelated
  crashes count as the symptom; this one makes them uninteresting, and
  enforces time / memory budgets.
USAGE:
  utils/reduce-test.py reduce <mode options> in.mlir -o out.mlir [--rounds 4]
      [--work <dir>] [--log <file>]
  utils/reduce-test.py script <mode options> -o t.sh   # for sea-reduce test=
  utils/reduce-test.py test   <mode options> <file>    # exit 1 interesting
  utils/reduce-test.py summary <log>                    # outcome table
  utils/reduce-test.py audit --mode result --result none \
      --config="--lower --vcgen-only" in.mlir --samples 20  # per pattern
MODES (--mode):
  crash            --config "<sea-vc args>" [--tool <exe>]
                   [--match <regex>] [--signal <n>|any]   (at least one)
  verdict-differs  --a "<args>" --b "<args>" [--a-result sat|unsat]
  slow             --a "<args>" --b "<args>" --metric time|mem|<counter>
                   --a-over <x> --b-under <y> [--min-ratio <r>]
                   time: seconds (A killed at a-over); mem: GB (max RSS;
                   A capped at a-over by RLIMIT_AS); <counter>: a
                   --seam-stats line, e.g. seam.rule.bv.ite-merge (add
                   --seam-stats to the args)
  result           --config "<args>" --result sat|unsat|unknown|none
                   [--status <n>]
COMMON: --sea-vc <exe> --sea-opt <exe> --sea-reduce <exe> (default: env
  SEA_VC / SEA_OPT / SEA_REDUCE, then build/bin next to this script, then
  the script's own directory (an install: bin/sea-reduce-test), then
  PATH); --timeout <s> (default 600) and --mem-cap <GB> (default 8): the
  safety limits of every run; --log <file>: one line per candidate;
  --cache <dir>: answers by file content (reduce: <work>/cache; the
  reduction tree tests many candidates equal to their parent).
  The file goes first: `sea-vc <file> <args>`. Write args that start
  with "--" as --a="--lower --seam" (with `=`: argparse).
OUTCOMES (every candidate, one --log line: `candidate: stage=<s>
  outcome=<o> kind=<diagnostic kind> interesting=0|1 cached=0|1 lines= ops=
  secs= <reason>`): verifier-rejected (MLIR verifier, inside sea-reduce:
  counted per stage from `sea-reduce.verifier-rejected`), verify-failed
  (sea-opt), sea-vc-error (status 1 or an error line: BMC form, vc.*,
  a pass postcondition; kind = the [tag] or the message head), crashed
  (signal), timeout, uninteresting, interesting. Only interesting is
  kept; a sea-vc error is never interesting (crash mode: unless it is the
  matched diagnostic).
OUTPUT (reduce, stdout `key: value`): reduce.input-lines / input-ops /
  output-lines / output-ops, reduce.rounds, reduce.time (seconds),
  reduce.stage: <stage> generated= cached= <outcome>=<n>... (one line per
  stage, then total; stages: global:<pattern>, cleanup[:part],
  tree:<pattern> (sea-reduction-tree; --stage-tests n caps its tests,
  the rest is left to the next round), tree:delete (upstream
  reduction-tree, op deletion only; --no-delete skips it)), reduce.kind: stage= outcome= kind= n=,
  reduce.recheck: interesting|NOT-INTERESTING (the output tested again:
  a slow case can be noise), reduce.log, reduce.output: <file>.
  test: exit 1 interesting, 0 not (stderr quiet unless --verbose).
  summary <log>: the stage table of a log.
  audit <mode options> --config/--a/--b ... file [--samples n]: each
  pattern applied once at n sampled sites (sea-reduce-apply op-index),
  each candidate classified: audit.stage: <pattern> ... (does a pattern
  make input sea-vc accepts?).
STATUS (reduce): 0 ok; 1 the input is not interesting or a tool failed.
"""

HERE = os.path.dirname(os.path.abspath(__file__))


def find_tool(name, given):
    if given:
        return given
    env = os.environ.get(name.upper().replace("-", "_"))
    if env:
        return env
    local = os.path.join(HERE, "..", "build", "bin", name)
    if os.path.exists(local):
        return os.path.abspath(local)
    # installed as <prefix>/bin/sea-reduce-test: the tools are its siblings
    sibling = os.path.join(HERE, name)
    if os.path.exists(sibling):
        return os.path.abspath(sibling)
    found = shutil.which(name)
    if not found:
        sys.exit(
            f"reduce-test: cannot find {name} (--{name} or ${name.upper().replace('-', '_')})"
        )
    return found


class Run:
    """One process run under a timeout and an address-space cap."""

    def __init__(self, argv, timeout, mem_gb):
        self.argv = argv
        out = tempfile.TemporaryFile()
        start = time.monotonic()

        def limit():
            if mem_gb:
                b = int(mem_gb * (1 << 30))
                resource.setrlimit(resource.RLIMIT_AS, (b, b))

        p = subprocess.Popen(
            argv,
            stdout=out,
            stderr=subprocess.STDOUT,
            preexec_fn=limit,
            start_new_session=True,
        )
        self.timed_out = False
        deadline = start + timeout if timeout else None
        while True:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
            if pid:
                break
            if deadline and time.monotonic() > deadline:
                self.timed_out = True
                os.killpg(p.pid, signal.SIGKILL)
                pid, status, ru = os.wait4(p.pid, 0)
                break
            time.sleep(0.02)
        p.returncode = 0  # reaped by wait4
        self.seconds = time.monotonic() - start
        self.max_rss_gb = ru.ru_maxrss / (1 << 20)  # KB -> GB
        self.signal = os.WTERMSIG(status) if os.WIFSIGNALED(status) else None
        self.status = os.WEXITSTATUS(status) if os.WIFEXITED(status) else None
        out.seek(0)
        self.output = out.read().decode(errors="replace")
        out.close()

    def result(self):
        m = re.findall(r"^result: (\S+)", self.output, re.M)
        return m[-1] if m else None

    def counter(self, name):
        m = re.findall(r"^" + re.escape(name) + r": (\d+)", self.output, re.M)
        return int(m[-1]) if m else None

    def errored(self):
        """An error run: a signal, a timeout, status 1, or an error line."""
        return (
            self.signal is not None
            or self.timed_out
            or self.status == 1
            or re.search(r"\berror:", self.output, re.I) is not None
        )

    def summary(self):
        return (
            f"status={self.status} signal={self.signal} "
            f"timeout={int(self.timed_out)} time={self.seconds:.2f} "
            f"rss={self.max_rss_gb:.3f}"
        )


def add_mode_args(ap):
    ap.add_argument(
        "--mode", required=True, choices=["crash", "verdict-differs", "slow", "result"]
    )
    ap.add_argument("--config", default="", help="sea-vc args (crash, result)")
    ap.add_argument("--tool", help="crash: the tool to run (default sea-vc)")
    ap.add_argument("--match", help="crash: regex the output must contain")
    ap.add_argument("--signal", help="crash: signal number, or any")
    ap.add_argument("--a", default="", help="configuration A args")
    ap.add_argument("--b", default="", help="configuration B args")
    ap.add_argument("--a-result", choices=["sat", "unsat"])
    ap.add_argument("--metric", help="slow: time | mem | <counter name>")
    ap.add_argument("--a-over", type=float)
    ap.add_argument("--b-under", type=float)
    ap.add_argument("--min-ratio", type=float)
    ap.add_argument("--result", choices=["sat", "unsat", "unknown", "none"])
    ap.add_argument("--status", type=int)
    ap.add_argument("--sea-vc")
    ap.add_argument("--sea-opt")
    ap.add_argument("--sea-reduce")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--mem-cap", type=float, default=8)
    ap.add_argument("--log")
    ap.add_argument("--cache", help="directory of answers by file content")
    ap.add_argument("--verbose", action="store_true")


def check_mode_args(a, ap):
    if a.mode == "crash" and not (a.match or a.signal):
        ap.error("crash: give --match and/or --signal")
    if a.mode in ("verdict-differs", "slow") and not (a.a or a.b):
        ap.error(f"{a.mode}: give --a and --b")
    if a.mode == "slow":
        if not a.metric or a.a_over is None or a.b_under is None:
            ap.error("slow: give --metric, --a-over and --b-under")
        # time / mem: B's budget is the smaller one. A counter may use a
        # ratio instead (--min-ratio) with any budgets.
        if a.metric in ("time", "mem") and a.b_under > a.a_over:
            ap.error("slow: --b-under must not exceed --a-over")
    if a.mode == "result" and not a.result:
        ap.error("result: give --result")


# Candidate outcomes, in the order of the summary table.
OUTCOMES = [
    "verifier-rejected",
    "verify-failed",
    "sea-vc-error",
    "crashed",
    "timeout",
    "uninteresting",
    "interesting",
]


def error_kind(output):
    """The kind of the first error a tool printed: a [tag] on the error
    line (sea::diag), else the message up to its first ':' ("BMC form"),
    else "status-1"."""
    for line in output.splitlines():
        m = re.search(r"\berror:\s*(.*)", line, re.I)
        if not m:
            continue
        tag = re.search(r"\[([\w.-]+)\]\s*$", line)
        if tag:
            return tag.group(1)
        return re.split(r"[:;,]", m.group(1).strip())[0][:40].strip() or "error"
    for line in output.splitlines():  # e.g. `sea-vc: no sea.program in input`
        if line.strip():
            return re.sub(r"^[\w-]+:\s*", "", line.strip())[:40]
    return "status-1"


def failure(r):
    """(outcome, kind) for a run that did not work, else None. A timeout
    is a failure here; slow mode decides itself when it is the budget."""
    if r.timed_out:
        return "timeout", ""
    if r.signal is not None:
        return "crashed", f"signal-{r.signal}"
    if r.status == 1 or re.search(r"\berror:", r.output, re.I):
        return "sea-vc-error", error_kind(r.output)
    return None


def interesting(a, path):
    """(ok, outcome, kind, why) for one candidate.

    A candidate that does not parse / verify (sea-opt), or that sea-vc
    refuses (an error, e.g. a BMC-form or vc.* input-form error, a pass
    postcondition), crashes or times out is not interesting in any mode:
    the only exceptions are the symptom a mode asks for (crash: the
    signal or the matched diagnostic; slow: A over its time / memory
    budget)."""
    sea_opt = find_tool("sea-opt", a.sea_opt)
    sea_vc = find_tool("sea-vc", a.sea_vc)
    v = Run([sea_opt, path, "-o", os.devnull], a.timeout, a.mem_cap)
    if v.status != 0:
        return False, "verify-failed", error_kind(v.output), "sea-opt"

    def vc(args, timeout=None, mem=None):
        return Run(
            [sea_vc, path] + shlex.split(args),
            a.timeout if timeout is None else timeout,
            a.mem_cap if mem is None else mem,
        )

    def bad(r, side=""):
        o, k = failure(r)
        return False, o, k, f"{side}{o} {r.summary()}"

    if a.mode == "crash":
        tool = a.tool or sea_vc
        r = Run([tool, path] + shlex.split(a.config), a.timeout, a.mem_cap)
        if r.timed_out:
            return bad(r)
        matched = a.match and re.search(a.match, r.output, re.M)
        if a.signal:
            if r.signal is None or (a.signal != "any" and r.signal != int(a.signal)):
                if failure(r):
                    return bad(r)
                return False, "uninteresting", "", "no-signal " + r.summary()
        if a.match and not matched:
            if failure(r):
                return bad(r)
            return False, "uninteresting", "", "no-match " + r.summary()
        # The symptom; another error before it would be an input-form
        # problem of the candidate, not the crash: refuse a kind that is
        # not the matched one.
        if not a.signal and failure(r):
            kind = failure(r)[1]
            if not re.search(a.match, kind) and not re.search(
                a.match, first_error_line(r.output)
            ):
                return bad(r)
        return True, "interesting", "", "crash " + r.summary()

    if a.mode == "result":
        r = vc(a.config)
        if failure(r) and not (
            a.status == 1 and r.status == 1 and r.signal is None and not r.timed_out
        ):
            return bad(r)
        if r.result() != a.result:
            return False, "uninteresting", "", f"result={r.result()} {r.summary()}"
        if a.status is not None and r.status != a.status:
            return False, "uninteresting", "", f"status {r.summary()}"
        return True, "interesting", "", f"result={r.result()} {r.summary()}"

    if a.mode == "verdict-differs":
        ra = vc(a.a)
        if failure(ra):
            return bad(ra, "a: ")
        res_a = ra.result()
        if res_a not in ("sat", "unsat") or (a.a_result and res_a != a.a_result):
            return False, "uninteresting", "", f"a: result={res_a}"
        rb = vc(a.b)
        if failure(rb):
            return bad(rb, "b: ")
        res_b = rb.result()
        if res_b not in ("sat", "unsat"):
            return False, "uninteresting", "", f"b: result={res_b}"
        return (
            res_a != res_b,
            "interesting" if res_a != res_b else "uninteresting",
            "",
            f"a={res_a} b={res_b}",
        )

    # slow: B first (cheap, and it must work), then A under its budget.
    m = a.metric
    if m == "time":
        rb = vc(a.b, timeout=a.b_under)
    elif m == "mem":
        rb = vc(a.b, mem=a.b_under)
    else:
        rb = vc(a.b)
    if failure(rb):
        if m == "time" and rb.timed_out:
            return False, "uninteresting", "", "b-over-time " + rb.summary()
        return bad(rb, "b: ")
    if m == "time":
        ra = vc(a.a, timeout=a.a_over)
        if not ra.timed_out and failure(ra):
            return bad(ra, "a: ")
        ok = ra.timed_out or ra.seconds > a.a_over
        va, vb = (a.a_over if ra.timed_out else ra.seconds), rb.seconds
    elif m == "mem":
        ra = vc(a.a, mem=a.a_over)
        # Over the cap: killed (a signal), `mem-limit: hit`, or an
        # allocation failure; else the measured peak.
        hit = (
            ra.signal is not None
            or "mem-limit: hit" in ra.output
            or re.search(r"bad_alloc|out of memory|Cannot allocate", ra.output)
            is not None
        )
        if not hit and failure(ra):
            return bad(ra, "a: ")
        ok = hit or ra.max_rss_gb > a.a_over
        va, vb = (a.a_over if hit else ra.max_rss_gb), rb.max_rss_gb
        if rb.max_rss_gb >= a.b_under:
            return False, "uninteresting", "", "b-over " + rb.summary()
    else:
        ra = vc(a.a)
        if failure(ra):
            return bad(ra, "a: ")
        va, vb = ra.counter(m), rb.counter(m)
        if va is None:
            return False, "uninteresting", "", f"a: no counter {m}"
        vb = vb or 0  # a counter that never fired is not printed
        ok = va > a.a_over
        if vb >= a.b_under:
            return False, "uninteresting", "", f"b-over {m}: a={va} b={vb}"
    if a.min_ratio and va < a.min_ratio * max(vb, 1e-9):
        return False, "uninteresting", "", f"ratio a={va} b={vb}"
    return (
        ok,
        "interesting" if ok else "uninteresting",
        "",
        f"a={va} b={vb} a:{ra.summary()} b:{rb.summary()}",
    )


def first_error_line(output):
    for line in output.splitlines():
        if re.search(r"\berror:", line, re.I):
            return line
    return ""


def mode_argv(a):
    """The mode options as a command line (for the generated script)."""
    out = ["--mode", a.mode]
    for k in [
        "config",
        "tool",
        "match",
        "signal",
        "a",
        "b",
        "a_result",
        "metric",
        "a_over",
        "b_under",
        "min_ratio",
        "result",
        "status",
        "timeout",
        "mem_cap",
        "log",
        "cache",
    ]:
        v = getattr(a, k)
        if v is not None and v != "":
            # --key=value: values may start with "--" (sea-vc args)
            out.append(f"--{k.replace('_', '-')}={v}")
    for k in ["sea_vc", "sea_opt"]:
        tool = find_tool(k.replace("_", "-"), getattr(a, k))
        out.append(f"--{k.replace('_', '-')}={os.path.abspath(tool)}")
    return out


def write_script(a, path):
    me = os.path.abspath(__file__)
    argv = [sys.executable, me, "test"] + mode_argv(a)
    with open(path, "w") as f:
        f.write("#!/bin/sh\n# generated by utils/reduce-test.py script\n")
        f.write("exec " + " ".join(shlex.quote(x) for x in argv) + ' "$@"\n')
    os.chmod(path, 0o755)


def size(path):
    lines = open(path).read().splitlines()
    ops = sum(
        1
        for l in lines
        if re.match(r"\s*(%[\w#:, %]+ = )?sea\.", l)
        and not re.match(r"\s*sea\.program", l)
    )
    return len(lines), ops


def log_line(a, stage, ok, outcome, kind, why, secs, cached, path):
    if not a.log:
        return
    lines, ops = size(path)
    with open(a.log, "a") as f:
        f.write(
            f"candidate: stage={stage or '-'} outcome={outcome} "
            f"kind={kind.replace(' ', '_') or '-'} interesting={int(ok)} "
            f"cached={int(cached)} lines={lines} ops={ops} "
            f"secs={secs:.1f} {why}\n"
        )


def cmd_test(a):
    t0 = time.monotonic()
    key = None
    if a.cache:
        # reduction-tree tests many variants equal to their parent (no
        # pattern matched the ops outside the range): answer those from
        # the cache. The key covers the mode options (not the stage, log or
        # cache: a cache can be reused by another run).
        h = hashlib.sha256(open(a.file, "rb").read())
        h.update(
            repr(
                [x for x in mode_argv(a) if not x.startswith(("--log=", "--cache="))]
            ).encode()
        )
        key = os.path.join(a.cache, h.hexdigest())
    cached = bool(key and os.path.exists(key))
    if cached:
        text = open(key).read()
        try:
            ok, outcome, kind, why = json.loads(text)
        except ValueError:  # the first format: "<0|1> <why>"
            ok, why = text.split(" ", 1)
            ok = ok == "1"
            outcome, kind = ("interesting" if ok else "uninteresting"), ""
    else:
        ok, outcome, kind, why = interesting(a, a.file)
        if key:
            os.makedirs(a.cache, exist_ok=True)
            with open(key, "w") as f:
                json.dump([ok, outcome, kind, why], f)
    log_line(a, a.stage, ok, outcome, kind, why, time.monotonic() - t0, cached, a.file)
    if a.verbose:
        print(
            f"interesting: {int(ok)}\noutcome: {outcome}\nkind: {kind}\nwhy: {why}",
            file=sys.stderr,
        )
    return 1 if ok else 0


def parse_log(path):
    """Candidates of a --log file: dicts with stage, outcome, kind,
    cached. Lines of the first format (no outcome=) are classified from
    their reason."""
    out = []
    for line in open(path):
        if not line.startswith("candidate:"):
            if line.startswith("stage-rejected:"):
                f = dict(x.split("=", 1) for x in line.split()[1:])
                out.append(
                    {
                        "stage": f["stage"],
                        "outcome": "verifier-rejected",
                        "kind": "mlir-verifier",
                        "cached": False,
                        "count": int(f["n"]),
                    }
                )
            continue
        f = dict(
            x.split("=", 1)
            for x in line.split()[1:]
            if "=" in x and not x.startswith(("a:", "b:"))
        )
        if "outcome" not in f:
            why = line
            f["stage"] = "-"
            f["kind"] = "-"
            if "does-not-verify" in why:
                f["outcome"] = "verify-failed"
            elif "status=1" in why or "-failed" in why:
                f["outcome"] = "sea-vc-error"
            elif f.get("interesting") == "1":
                f["outcome"] = "interesting"
            else:
                f["outcome"] = "uninteresting"
            f["cached"] = "1" if " cached " in why else "0"
        out.append(
            {
                "stage": f["stage"],
                "outcome": f["outcome"],
                "kind": f.get("kind", "-"),
                "cached": f["cached"] == "1",
                "count": 1,
            }
        )
    return out


def print_summary(cands, prefix="summary"):
    """One `key: value` line per stage: candidates by outcome; then the
    sea-vc error kinds."""
    stages = []
    for c in cands:
        if c["stage"] not in stages:
            stages.append(c["stage"])
    # Outcome columns count each distinct candidate once: the verifier's
    # rejections (never tested) and the candidates tested; `cached` are
    # repeats answered from the cache (reduction-tree regenerates its
    # parent and earlier variants).
    print(f"{prefix}.columns: stage generated cached " + " ".join(OUTCOMES))
    for s in stages + ["total"]:
        cs = [c for c in cands if s == "total" or c["stage"] == s]
        n = {
            o: sum(c["count"] for c in cs if c["outcome"] == o and not c["cached"])
            for o in OUTCOMES
        }
        gen = sum(c["count"] for c in cs)
        cached = sum(1 for c in cs if c["cached"])
        print(
            f"{prefix}.stage: {s} generated={gen} cached={cached} "
            + " ".join(f"{o}={n[o]}" for o in OUTCOMES)
        )
    kinds = {}
    for c in cands:
        if (
            c["outcome"] in ("sea-vc-error", "verify-failed", "crashed")
            and not c["cached"]
        ):
            k = (c["stage"], c["outcome"], c["kind"])
            kinds[k] = kinds.get(k, 0) + 1
    for (s, o, k), n in sorted(kinds.items()):
        print(f"{prefix}.kind: stage={s} outcome={o} kind={k} n={n}")


def cmd_summary(a):
    print_summary(parse_log(a.log_file))
    return 0


def run_stage(a, sea_reduce, env, script, stage, cur, nxt, pass_args):
    args = [sea_reduce, cur, "-o", nxt] + pass_args
    p = subprocess.run(args, env=env, capture_output=True, text=True)
    m = re.search(r"sea-reduce.verifier-rejected: (\d+)", p.stderr)
    if a.log:
        with open(a.log, "a") as f:
            f.write(f"stage-rejected: stage={stage} n={m.group(1) if m else 0}\n")
    if p.returncode != 0:
        print(f"reduce.error: sea-reduce failed in stage {stage}")
        sys.stderr.write(p.stderr[-4000:])
        return False
    return True


def cmd_reduce(a):
    sea_reduce = find_tool("sea-reduce", a.sea_reduce)
    work = a.work or tempfile.mkdtemp(prefix="sea-reduce-")
    os.makedirs(work, exist_ok=True)
    if not a.log:
        a.log = os.path.join(work, "candidates.log")
    script = os.path.join(work, "test.sh")
    if not a.cache:
        a.cache = os.path.join(work, "cache")
    write_script(a, script)
    env = dict(os.environ, TMPDIR=work)
    t0 = time.monotonic()
    cur = os.path.join(work, "round0.mlir")
    shutil.copyfile(a.file, cur)
    ok, outcome, kind, why = interesting(a, cur)
    lines0, ops0 = size(cur)
    print(f"reduce.input-lines: {lines0}\nreduce.input-ops: {ops0}")
    print(f"reduce.input-test: {outcome} {kind} {why}")
    if not ok:
        print("reduce.error: the input is not interesting")
        return 1

    def test(stage):
        return f"test={script} test-arg=--stage={stage}"

    # Per round: each pattern everywhere at once (one test each), cleanup
    # (whole, then its parts: unreachable blocks can carry the symptom,
    # sea-vc encodes them), then sea-reduction-tree once per pattern (the
    # upstream search over ranges of ops, patterns only), cleanup, and
    # last upstream reduction-tree with no pattern (op-range deletion:
    # slow on large CFGs, so after the patterns shrank the program;
    # --no-delete skips it), cleanup.
    stages = [
        (
            f"global:{p}",
            [
                f"--opt-reduction-pass=opt-pass=sea-reduce-apply{{pattern={p} erase-unreachable=true}} "
                + test(f"global:{p}")
            ],
        )
        for p in ("drop-assume", "select-branch", "memory-bypass", "load-to-nondet")
    ]
    cleanup = [
        (
            f"cleanup{':' + o if o else ''}",
            [
                f"--opt-reduction-pass=opt-pass=sea-reduce-cleanup{{{o}}} "
                + test(f"cleanup{':' + o if o else ''}")
            ],
        )
        for o in ("", "blocks=false", "dead-ops=false")
    ]
    stages += cleanup
    for p in (
        "erase-dead",
        "memory-bypass",
        "drop-assume",
        "select-branch",
        "load-to-nondet",
        "forward-operand",
        "zero-const",
    ):
        stages.append(
            (
                f"tree:{p}",
                [
                    f"--sea-reduction-tree=pattern={p} "
                    f"max-tests={a.stage_tests} " + test(f"tree:{p}")
                ],
            )
        )
    stages += cleanup
    if not a.no_delete:
        stages.append(
            (
                "tree:delete",
                [
                    "--sea-patterns=none",
                    "--reduction-tree=traversal-mode=0 " + test("tree:delete"),
                ],
            )
        )
        stages += cleanup
    rounds = 0
    step = 0
    for rounds in range(1, a.rounds + 1):
        before = os.path.getsize(cur)
        for stage, pass_args in stages:
            step += 1
            nxt = os.path.join(work, f"step{step}.mlir")
            if not run_stage(a, sea_reduce, env, script, stage, cur, nxt, pass_args):
                return 1
            cur = nxt
        l, o = size(cur)
        print(
            f"reduce.round: {rounds} lines={l} ops={o} "
            f"secs={time.monotonic() - t0:.0f}",
            flush=True,
        )
        if os.path.getsize(cur) >= before:
            break
    shutil.copyfile(cur, a.output)
    lines1, ops1 = size(a.output)
    ok, outcome, kind, why = interesting(a, a.output)
    print(f"reduce.output-lines: {lines1}\nreduce.output-ops: {ops1}")
    print(f"reduce.rounds: {rounds}")
    print(f"reduce.time: {time.monotonic() - t0:.0f}")
    print_summary(parse_log(a.log), "reduce")
    print(f"reduce.recheck: {'interesting' if ok else 'NOT-INTERESTING'} ({why})")
    print(f"reduce.log: {a.log}")
    print(f"reduce.output: {a.output}")
    return 0 if ok else 1


def cmd_audit(a):
    """Each pattern applied once, at sampled ops of the input, and each
    candidate classified by the test (verify, sea-vc errors, ...): whether
    a pattern makes input sea-vc accepts, independent of the search."""
    import concurrent.futures as cf
    import random

    sea_opt = find_tool("sea-opt", a.sea_opt)
    work = a.work or tempfile.mkdtemp(prefix="sea-audit-")
    os.makedirs(work, exist_ok=True)
    patterns = (
        a.patterns.split(",")
        if a.patterns
        else [
            "memory-bypass",
            "drop-assume",
            "select-branch",
            "load-to-nondet",
            "forward-operand",
            "zero-const",
            "erase-dead",
        ]
    )
    jobs = []
    for p in patterns:
        r = subprocess.run(
            [
                sea_opt,
                a.file,
                "-o",
                os.devnull,
                f"--sea-reduce-apply=pattern={p} report=true",
            ],
            capture_output=True,
            text=True,
        )
        idx = [int(m) for m in re.findall(r"match: index=(\d+)", r.stderr)]
        rnd = random.Random(a.seed)
        pick = sorted(rnd.sample(idx, min(a.samples, len(idx))))
        print(
            f"audit.matches: pattern={p} sites={len(idx)} sampled={len(pick)}",
            flush=True,
        )
        for i in pick:
            jobs.append((p, i))

    def one(job):
        p, i = job
        cand = os.path.join(work, f"{p}.{i}.mlir")
        r = subprocess.run(
            [
                sea_opt,
                a.file,
                "-o",
                cand,
                f"--sea-reduce-apply=pattern={p} op-index={i}",
            ],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            return (
                p,
                i,
                False,
                "verifier-rejected",
                error_kind(r.stdout + r.stderr),
                "sea-opt",
            )
        res = interesting(a, cand)
        os.remove(cand)
        return (p, i) + tuple(res)

    cands = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for p, i, _ok, outcome, kind, _why in ex.map(one, jobs):
            print(
                f"audit.candidate: pattern={p} index={i} outcome={outcome} "
                f"kind={kind.replace(' ', '_') or '-'}",
                flush=True,
            )
            cands.append(
                {
                    "stage": p,
                    "outcome": outcome,
                    "kind": kind.replace(" ", "_") or "-",
                    "cached": False,
                    "count": 1,
                }
            )
    print_summary(cands, "audit")
    return 0


def main():
    if "--agent" in sys.argv[1:]:
        print(AGENT, end="")
        return 0
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("test")
    add_mode_args(t)
    t.add_argument("--stage", help="label for the log (sea-reduce stage)")
    t.add_argument("file")
    s = sub.add_parser("script")
    add_mode_args(s)
    s.add_argument("-o", dest="output", required=True)
    r = sub.add_parser("reduce")
    add_mode_args(r)
    r.add_argument("file")
    r.add_argument("-o", dest="output", required=True)
    r.add_argument("--rounds", type=int, default=4)
    r.add_argument("--work", help="directory for rounds and temp files")
    r.add_argument(
        "--stage-tests",
        type=int,
        default=0,
        help="sea-reduction-tree: max tests per stage (0: none)",
    )
    r.add_argument(
        "--no-delete",
        action="store_true",
        help="skip upstream op-range deletion (reduction-tree)",
    )
    u = sub.add_parser("audit")
    add_mode_args(u)
    u.add_argument("file")
    u.add_argument("--patterns", help="comma-separated (default: all)")
    u.add_argument("--samples", type=int, default=20)
    u.add_argument("--seed", type=int, default=1)
    u.add_argument("--jobs", type=int, default=8)
    u.add_argument("--work")
    m = sub.add_parser("summary")
    m.add_argument("log_file")
    a = ap.parse_args()
    if a.cmd == "summary":
        return cmd_summary(a)
    check_mode_args(a, {"test": t, "script": s, "reduce": r, "audit": u}[a.cmd])
    if a.log:
        a.log = os.path.abspath(a.log)
    if a.cmd == "test":
        return cmd_test(a)
    if a.cmd == "script":
        write_script(a, a.output)
        return 0
    if a.cmd == "audit":
        return cmd_audit(a)
    return cmd_reduce(a)


if __name__ == "__main__":
    sys.exit(main())
