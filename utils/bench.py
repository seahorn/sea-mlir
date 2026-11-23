#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Minimal sea-vc benchmark tool (stdlib + PyYAML). `bench.py --agent` for the guide.

Named runs are yaml option files (SeaHorn's `sea yama -y`:
`include:` overlays), per-file runs under CPU/memory rlimits, tools print
`BRUNCH_STAT <key> <value>` lines that become CSV columns.
"""

import argparse
import concurrent.futures as cf
import csv
import datetime
import glob
import hashlib
import json
import os
import platform
import re
import resource
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time

import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.abspath(
    os.path.expanduser(
        os.environ.get(
            "SEA_BENCH_HOME",
            os.path.join(
                os.environ.get("XDG_CACHE_HOME", "~/.cache"), "sea-mlir-bench"
            ),
        )
    )
)

AGENT = """bench.py agent guide (plain, terse)
WHAT: benchmark sea-vc over .mlir corpora; named runs are yaml option files.
LAYOUT: bench/jobsets/<name>.yaml, bench/configs/<name>.yaml (in the repo);
  corpora $SEA_BENCH_HOME/corpus/<jobset>/ and results
  $SEA_BENCH_HOME/results/<date>-<name>/ (default ~/.cache/sea-mlir-bench).
  A yaml may `include: [other, ...]` (like yama; later keys override, `args` concatenate).
  jobset: source: seahorn (sea, build, yaml, memopt, sea-args: [option, ...] after the yaml, jobs: [{dir, names}]) | copy (from: dir, files).
  config: args (sea-vc args), solver, timeout (s, CPU), mem (MB).
COMMANDS:
  bench.py list
  bench.py export <jobset> [--par N] [--force]   .mlir + <job>.json (exact command, bitcode sha256); cached by bitcode hash
  bench.py run <jobset> <config>... [--par N] [--timeout S] [--reps K] [--sea-vc PATH] [--name N] [--jobs a,b]
  bench.py compare <dirA> [<dirB>] [-a CONFIG] [-b CONFIG]   (dir: path, name under results/, or `latest`)
OUTPUT of run: stats.csv (job config rep wall cpu status verdict + every BRUNCH_STAT key),
  logs/<config>/<job>.<rep>.{out,err}, manifest.yaml (sea-vc sha256, git rev/dirty, configs, host, date), rows.jsonl.
  verdict: sat unsat unknown timeout memout budget (rc 8) error inb-violation (rc 4) replay-mismatch (rc 2)
  premise-failed (rc 4 with a `premise.failed:` line: the replay failed a premise assert,
  --symbolic-size=assert, not the property; inconclusive),
  promise-broken (rc 4 with a `vc.ack-stride-promise-broken:` line: the replay breaks a front-end GEP promise
  the stride rule trusts at an access; the answer is that run),
  lowering-mismatch (rc 6: --replay-original, the input program does not fail the lowered sat's assertion),
  original-memory-error (rc 4 with replay.original match=no: the input program makes a memory-safety violation),
  check-invalid (rc 5: --seam-check / --narrow-check found an invalid rewrite), check-unproven (rc 7:
  --narrow-check left a narrowed read index unproven).
  budget: --max-nodes / --max-rss exceeded (`result: unknown`, `budget: hit`); gated like unknown / memout:
  a failure of a sat / unsat job (unsolved), allowed on unknown-ok.
  Classified by what sea-vc printed: the verdict is its one `result:` line of stdout, the settled answer (text
  or --format=jsonl; a sat only after its replay confirms it, `solver-answer:` is the solver's; a split's parts
  and BRUNCH_STAT result do not count; column `result`; the last such line if an older sea-vc printed two):
  rc 0/3 give it (rc 3: `result: unknown`, e.g. after a mem-limit hit in the replay; no `result:` line: error).
  rc 1 after `solver-answer: sat` with error vc.replay-impossible (the in-place
  interpreter cannot run the model: a memory update without `inplace`) is verdict sat, column replay=impossible;
  any other rc 1 after a verdict is error; memout needs bad_alloc / out of memory, or rc 16 / abort / segv
  with no verdict printed. A timeout is a timeout even after a printed verdict (stats `result` keeps it).
compare prints `key: value` lines; verdict disagreements first, `DISAGREE-SAT-UNSAT` is a soundness bug.
--no-record on gate: no gate record ($SEA_BENCH_HOME/gates); results and logs stay in $SEA_BENCH_HOME/results (attest reads them).
SUITES (benchmark repos outside sea-mlir, docs/Benchmarks.md): suite roots = $SEA_BENCH_SUITES (colon-separated)
  or bench/work/<name>/ (gitignored; `bench.py connect` / `just bench-connect` fills it from bench/sources.yaml,
  overlaid by bench/sources.local.yaml and $SEA_BENCH_SOURCE_<NAME>; local path -> symlink, URL -> clone / pull).
  A family is <root>/<family>/{manifest.yaml, <job>.mlir}; manifest: defaults {args, timeout, mem},
  jobs.<job> {expected sat|unsat|unknown-ok, tier smoke|quick|full|hard, time (s), args, tags, export, counters,
  replay: impossible (the only value: a sat of this job may end in vc.replay-impossible),
  premise-class: B | C (absent: A, the --seam premises hold on every run; B: some counterexample keeps them,
  sat required; C: every counterexample violates them: unsat, or a sat with a premise signal; docs/Benchmarks.md,
  premise classes)}.
  Counter expectations (completeness): counters: {<stat>: {max: N | min: N | exact: N, why: text, seeded: stamp}};
  <stat> = a BRUNCH_STAT key or a `key: value` stdout line of sea-vc (seam.rule.* needs --seam-stats in args),
  or <line>.<field> for a `<line>: k=v k=v ...` line (vc.box-all.expanded, vc.expand-memmove.kept-over-k: the
  lowering's form; a job without the line has the counter missing). An exact key match wins.
  Effective = defaults.counters, then each counter-rules entry {jobs: REGEX over the job name, counters, why}
  that matches, then the job's own (a null value removes an inherited bound). Checked on config `default` only.
  Tiers: smoke <5 s, quick <60 s, full <900 s, hard (tracked, not gated); a tier gate runs it and the lower ones.
  bench.py connect
  bench.py suites [SEL...]                     families, jobs per tier and expected verdict (SEL: suite, family or suite/family)
  bench.py gate <tier> [--config default[,default+CFG,CFG...]] [--suite SEL]... [--par N] [--sea-vc PATH]
       [--slow F] [--jobs REGEX] [--tag T] [--no-counters] [--no-record]
    config `default` = the manifest's defaults (+ job args); `default+CFG` adds bench/configs/CFG.yaml's args
    and solver; CFG alone replaces the defaults. Job timeout = min(config timeout, tier cap: smoke 60,
    quick 300, else the config's).
    Every run gets --replay-original when the sea-vc has it (`replay-original: on|off` line; --no-replay-original:
    off): a sat is also replayed on the program before the lowering.
    FAILS (exit 1) on: verdict != expected (sat/unsat jobs; timeout/unknown counts), replay-mismatch (rc 2),
    lowering-mismatch (rc 6), original-memory-error (rc 4 under --replay-original) on a gated job,
    replay-impossible (a sat whose model cannot be replayed) unless the job has `replay: impossible`,
    error / inb-violation / premise-failed / promise-broken on a gated job (unknown-ok too; memout there is allowed), sat vs unsat between two configs (DISAGREE-SAT-UNSAT),
    and (`FAIL: counter ...`, counted apart as counter-failures) a solved job of config `default` whose
    counter breaks its bound or is missing from the output (--no-counters: skip). --tag T: only jobs tagged T
    (`canary`: the small cross-family set, `just bench-canary`).
    premise-class B / C jobs pass a sat whose replay failed in a way a premise violation explains: rc 2 with
    `replay: assert-failed`, a `seam.inb-violation:` line and error vc.replay-mismatch (`premise-unconfirmed:
    <job> ... log=... rerun: ...`) or rc 4 (`premise-inb-violation:`: an access outside an object, verdict
    inb-violation, or a run that breaks a GEP promise the stride rule trusts, verdict promise-broken); a run with
    `replay: no-clobber-violated:` is never explained. Unsat fails on B, passes on C. A confirmed sat whose
    original-program run left its object (replay.original status=memory-error) counts as left-object. On C a
    sat needs one of these signals:
    a clean sat fails (`misclassified`, declare B), and DISAGREE-SAT-UNSAT is waived only when every sat run
    of the job has a signal. Undeclared jobs are
    judged as before; a run with one of these signals gets `premise may fail on this job: declare it` (on its
    FAIL line, or a `premise-hint:` line). Summary: `premises: declared-runs=N left-object=N unconfirmed=N
    inb-violation=N unsat-c=N hints=N`. rows.jsonl: replay_original_status, replay_status, seam_inb,
    replay_no_clobber, replay_mismatch_diag.
    Reports (no failure): `slow:` wall > F x the manifest time (default 2, and +1 s), `solved:` an unknown-ok job solved,
    `replayed:` a `replay: impossible` job whose sat replayed (the field is stale); `replay-impossible: N allowed=M`.
    Writes logs to $SEA_BENCH_HOME/results/<stamp>-gate-<tier>/ and appends the record to
    $SEA_BENCH_HOME/gates/<date>-<commit>.yaml. Output: `key: value` lines, `FAIL:` per failure, `gate: pass|fail`.
  gate / establish: --par 4 by default; --min-free-gb and --max-load tune when jobs start (`wait:` line);
    --mem MB overrides the manifests' cap (16 GB).
  bench.py establish <SEL>... [--par N] [--sea-vc PATH] [--jobs REGEX] [--resume RESULTSDIR [--no-run]]
       [--record-counters KEY,... [--counters-only]]
    runs the default configuration and writes expected / tier / time into the manifests (curation; review the
    diff in the benchmark repo; sat/unsat -> expected, timeout/unknown/memout/budget -> unknown-ok + hard).
    --record-counters: for each solved job and KEY without a deliberate bound, writes the observed value as
    {max: value, seeded: <date> <sea-vc sha>} (replacing an older seeded bound); a deliberate bound (no
    `seeded`) is never changed: a run that breaks it prints COUNTER-OVER. --counters-only: only that.
    A sat that cannot be replayed prints `replay-impossible:`; establish never writes `replay` (curation).
    A premise signal on an undeclared job prints `premise-hint:`; establish never writes `premise-class`. A
    declared job's explained sat (rc 2 / 4, as in gate) is recorded as sat; unsat on a class B job prints
    PREMISE-B-UNSAT and leaves `expected` as is; a clean sat on a class C job prints PREMISE-C-MISCLASSIFIED.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def load_yaml(kind, name):
    """bench/<kind>/<name>.yaml with includes resolved (args concatenate)."""
    # $SEA_BENCH_HOME/bench/<kind>/ (local overrides) before the repo's
    path = os.path.join(HOME, "bench", kind, name + ".yaml")
    if not os.path.exists(path):
        path = os.path.join(REPO, "bench", kind, name + ".yaml")
    if not os.path.exists(path):
        sys.exit(f"bench: no {kind[:-1]} '{name}' ({path})")
    d = yaml.safe_load(open(path)) or {}

    def expand(value):
        if isinstance(value, str):
            return re.sub(
                r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}",
                lambda m: os.environ.get(m.group(1), m.group(2) or m.group(0)),
                value,
            )
        if isinstance(value, list):
            return [expand(x) for x in value]
        if isinstance(value, dict):
            return {k: expand(v) for k, v in value.items()}
        return value

    d = expand(d)
    inc = d.pop("include", [])
    out = {}
    for i in [inc] if isinstance(inc, str) else inc:
        sub = load_yaml(kind, i)
        out.update({k: v for k, v in sub.items() if k != "args"})
        out["args"] = out.get("args", []) + sub.get("args", [])
    out.update({k: v for k, v in d.items() if k != "args"})
    if "args" in d or "args" in out:
        out["args"] = out.get("args", []) + d.get("args", [])
    return out


def names(kind):
    return sorted(
        {
            os.path.basename(p)[:-5]
            for d in (HOME, REPO)
            for p in glob.glob(os.path.join(d, "bench", kind, "*.yaml"))
        }
    )


def corpus_dir(jobset):
    return os.path.join(HOME, "corpus", jobset)


# ---------------------------------------------------------------- list
def cmd_list(a):
    print("bench-home:", HOME)
    for n in names("jobsets"):
        d = load_yaml("jobsets", n)
        have = len(glob.glob(os.path.join(corpus_dir(n), "*.mlir")))
        print(
            f"jobset: {n} source={d.get('source')} exported={have} widen={d.get('widen')} :: {d.get('description', '')}"
        )
    for n in names("configs"):
        d = load_yaml("configs", n)
        print(
            f"config: {n} args={' '.join(d.get('args', []))} solver={d.get('solver')} timeout={d.get('timeout')} mem={d.get('mem')}"
        )


# ---------------------------------------------------------------- export
def seahorn_jobs(js):
    for g in js["jobs"]:
        for n in g["names"]:
            d = os.path.join(js["build"], "seahorn", g["dir"], n)
            bcs = glob.glob(os.path.join(d, "llvm-ir", "*.ir", "*.ir.bc"))
            yield n, g["dir"], (bcs[0] if bcs else None)


def export_one(js, jobset, n, gdir, bc, force):
    out = os.path.join(corpus_dir(jobset), n + ".mlir")
    meta = out[:-5] + ".json"
    if bc is None:
        return n, "no-bitcode", 0
    bch = sha256(bc)
    build = js["build"]
    ys = []
    for y in js["yaml"] + [f"seahorn/{gdir}/{n}/sea.yaml"]:
        if os.path.exists(os.path.join(build, y)):
            ys += ["-y", y]
    tmp = tempfile.mkdtemp(prefix=n + ".", dir=os.path.join(HOME, "tmp"))
    cmd = (
        [js["sea"], "yama", "--yforce"]
        + ys
        + [
            "fpf",
            "--horn-bmc-engine=sea-mlir",
            "--horn-sea-vc=/bin/true",
            f"--horn-sea-mlir-out={out}",
            f"--temp-dir={tmp}",
        ]
    )
    if not js.get("memopt", True):
        cmd.append("--horn-shadow-mem-optimize=false")
    # options after the yaml files (they override them)
    cmd += [str(x) for x in js.get("sea-args", [])]
    cmd.append(bc)
    key = hashlib.sha256(
        (bch + " ".join(cmd).replace(tmp, "") + out).encode()
    ).hexdigest()
    if not force and os.path.exists(out) and os.path.exists(meta):
        if json.load(open(meta)).get("key") == key:
            shutil.rmtree(tmp, ignore_errors=True)
            return n, "cached", 0
    t = time.monotonic()
    try:
        p = subprocess.run(
            cmd,
            cwd=build,
            capture_output=True,
            text=True,
            timeout=js.get("export-timeout", 1800),
        )
        rc, log = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        rc, log = -9, "export timeout"
    sec = time.monotonic() - t
    shutil.rmtree(tmp, ignore_errors=True)
    ok = rc == 0 and os.path.exists(out)
    json.dump(
        {
            "key": key,
            "ok": ok,
            "rc": rc,
            "seconds": round(sec, 1),
            "bitcode": bc,
            "bitcode_sha256": bch,
            "cwd": build,
            "command": " ".join(shlex.quote(c) for c in cmd),
            "memopt": js.get("memopt", True),
            "date": datetime.datetime.now().isoformat(timespec="seconds"),
            "log_tail": log[-2000:] if not ok else "",
        },
        open(meta, "w"),
        indent=1,
    )
    return n, "ok" if ok else f"FAILED rc={rc}", sec


def cmd_export(a):
    js = load_yaml("jobsets", a.jobset)
    cd = corpus_dir(a.jobset)
    os.makedirs(cd, exist_ok=True)
    os.makedirs(os.path.join(HOME, "tmp"), exist_ok=True)
    t0 = time.monotonic()
    res = []
    if js["source"] == "copy":
        src = js["from"]
        files = js.get("files") or sorted(
            os.path.basename(p)[:-5] for p in glob.glob(src + "/*.mlir")
        )
        for n in files:
            shutil.copy2(f"{src}/{n}.mlir", f"{cd}/{n}.mlir")
            json.dump(
                {
                    "copied_from": f"{src}/{n}.mlir",
                    "sha256": sha256(f"{cd}/{n}.mlir"),
                    "note": "not re-exported; origin command unknown/in scratchpad",
                },
                open(f"{cd}/{n}.json", "w"),
                indent=1,
            )
            res.append((n, "copied", 0))
    else:
        with cf.ThreadPoolExecutor(a.par) as ex:
            futs = [
                ex.submit(export_one, js, a.jobset, n, g, bc, a.force)
                for n, g, bc in seahorn_jobs(js)
            ]
            for f in cf.as_completed(futs):
                r = f.result()
                res.append(r)
                print(f"export: {r[0]} {r[1]} {r[2]:.0f}s", flush=True)
    st = {}
    for _, s, _ in res:
        st[s.split()[0]] = st.get(s.split()[0], 0) + 1
    print(f"jobs: {len(res)}")
    print("status:", " ".join(f"{k}={v}" for k, v in sorted(st.items())))
    print(f"export-time: {time.monotonic() - t0:.0f}")
    print(f"corpus: {cd}")
    for n, s, _ in sorted(res):
        if s.startswith("FAILED") or s == "no-bitcode":
            print(f"failed: {n} {s}")


# ---------------------------------------------------------------- run
def git_state():
    def g(*x):
        return subprocess.run(
            ["git", "--no-optional-locks", "-C", REPO] + list(x),
            capture_output=True,
            text=True,
        ).stdout.strip()

    return {
        "rev": g("rev-parse", "HEAD"),
        "branch": g("branch", "--show-current"),
        "dirty": bool(g("status", "--porcelain", "-uno")),
    }


def as_text(out):
    """sea-vc stdout as text: a --format=jsonl record becomes its `text` (the text format's rendering)."""
    if "{" not in out:
        return out
    lines = []
    for line in out.splitlines():
        if line.startswith("{"):
            try:
                rec = json.loads(line)
            except ValueError:
                rec = None
            if isinstance(rec, dict) and isinstance(rec.get("text"), str):
                lines.append(rec["text"])
                continue
        lines.append(line)
    return "\n".join(lines) + "\n"


def final_result(out):
    """The verdict sea-vc printed: its `result:` line, one per run, the settled answer (the last one, for an
    older sea-vc that printed `result: sat` before its replay). Not the BRUNCH_STAT result, which a split
    prints per part (the split's own `result:` has none). None if the run printed no verdict."""
    m = re.findall(r"^result: (\S+)", out, re.M)
    return m[-1] if m else None


def solver_answer(out):
    """The solver's answer (`solver-answer:`, before the replay settles a sat); None if not printed."""
    m = re.findall(
        r'^solver-answer: (\S+)|"key":"solver-answer","value":"(\w+)"', out, re.M
    )
    return (m[-1][0] or m[-1][1]) if m else None


def parse_stats(text, stats):
    for line in text.splitlines():
        if line.startswith("BRUNCH_STAT "):
            f = line.split(" ")
            if len(f) == 3:
                stats[f[1]] = f[2].strip()


def run_one(vc, mlir, cfg, cfgname, job, rep, outdir, timeout, mem):
    args = [vc, mlir] + cfg.get("args", [])
    if cfg.get("solver"):
        args.append("--solver=" + cfg["solver"])
    args.append("--brunch-stats")

    def limits():  # as brunch: CPU and address-space rlimits
        if mem:
            resource.setrlimit(resource.RLIMIT_AS, (mem << 20, mem << 20))
        if timeout:
            resource.setrlimit(resource.RLIMIT_CPU, (timeout, timeout + 5))

    ld = os.path.join(outdir, "logs", cfgname)
    os.makedirs(ld, exist_ok=True)
    base = f"{ld}/{job}.{rep}"
    t = time.monotonic()
    with open(base + ".out", "w") as fo, open(base + ".err", "w") as fe:
        p = subprocess.Popen(args, stdout=fo, stderr=fe, preexec_fn=limits)
        killed = []
        timer = (
            threading.Timer(2 * timeout + 30, lambda: (killed.append(1), p.kill()))
            if timeout
            else None
        )
        if timer:
            timer.start()
        _, status, ru = os.wait4(p.pid, 0)
        if timer:
            timer.cancel()
    wall = time.monotonic() - t
    rc = os.waitstatus_to_exitcode(status)
    stats = {}
    out, err = as_text(open(base + ".out").read()), open(base + ".err").read()
    parse_stats(out, stats)
    parse_stats(err, stats)
    # --replay-original: yes | no | undetermined | skipped (the first line)
    m = re.search(
        r"^replay\.original: (?:status=\S+ at=\S+ match=(\S+)|(skipped))", out, re.M
    )
    replay_original = (m.group(1) or m.group(2)) if m else None
    # premise signals (docs/Benchmarks.md, premise classes): the original program's run status
    # (`memory-error`: it left its object), the lowered replay's status, and the A-INB fallback check
    # after a replay mismatch (`seam.inb-violation: undetermined (...)`, or found: its message, rc 4)
    m = re.search(r"^replay\.original: status=(\S+)", out, re.M)
    replay_original_status = m.group(1) if m else None
    m = re.search(r"^replay: ([a-z-]+)$", out, re.M)
    replay_status = m.group(1) if m else None
    m = re.search(r"^seam\.inb-violation: (undetermined\b)?", out, re.M)
    seam_inb = ("undetermined" if m.group(1) else "found") if m else None
    # a mismatch explained by a Mem-SSA no-clobber claim is not a premise question
    replay_no_clobber = (
        re.search(r"^replay: no-clobber-violated:", out, re.M) is not None
    )
    replay_mismatch_diag = "[vc.replay-mismatch]" in err + out
    # classify by what sea-vc printed, not by the exit code alone: a run that
    # printed a verdict and then exited 1 ended in an error after its answer
    # (never a memout); a resource outcome needs evidence of one
    printed = final_result(out) or stats.get("result")
    if printed is not None:
        stats["result"] = printed  # the run's verdict, not a split part's
    replay = None  # `impossible`: sat whose model the interpreter cannot run
    oom = (
        "bad_alloc" in err
        or re.search(r"out of memory|cannot allocate memory", err, re.I) is not None
    )
    if killed or rc == -signal.SIGXCPU or rc == -signal.SIGKILL:
        verdict = "timeout"
    elif rc in (0, 3):
        verdict = printed or "error"
    elif rc == 8 and printed == "unknown":
        # --max-nodes / --max-rss: a budget stop (`budget: hit`), not a solver
        # answer; like unknown / memout for the gate
        verdict = "budget"
    elif rc == 4 and replay_original == "no":
        # --replay-original: the input program makes a memory-safety
        # violation and fails no assertion of the lowered sat
        verdict = "original-memory-error"
    elif rc == 4 and re.search(r'^premise\.failed:|"kind":"premise"', out, re.M):
        # --symbolic-size=assert: the replay failed a premise assert (an
        # allocation exceeded its room), not the property
        verdict = "premise-failed"
    elif rc == 4 and re.search(
        r'^vc\.ack-stride-promise-broken:|"key":"vc\.ack-stride-promise-broken"',
        out,
        re.M,
    ):
        # --ack-stride: the replay breaks a front-end GEP promise the stride
        # rule trusts, at an access; the answer is that run
        verdict = "promise-broken"
    elif rc == 4:
        verdict = "inb-violation"
    elif rc == 2:
        verdict = "replay-mismatch"
    elif rc == 6:
        verdict = "lowering-mismatch"
    elif rc == 5:
        # --seam-check / --narrow-check: a rule firing or a narrowed read
        # index is invalid under its premises (a rewriting bug)
        verdict = "check-invalid"
    elif rc == 7:
        # --narrow-check: a narrowed read index whose premise bound is not
        # proven (the narrowing is not discharged)
        verdict = "check-unproven"
    elif (
        rc == 1
        and (printed == "sat" or solver_answer(out) == "sat")
        and "vc.replay-impossible" in err + out
    ):
        # sat with an exact VC whose model the in-place interpreter cannot run
        # (a memory update without `inplace`, docs/design/Memory.md): the
        # verdict stands, the replay is impossible (manifest `replay: impossible`)
        verdict, replay = "sat", "impossible"
    elif oom or (
        printed is None and rc in (16, -signal.SIGABRT, -signal.SIGSEGV)
    ):  # 16: Yices' own out-of-memory exit; an abort / segv under RLIMIT_AS
        verdict = "memout"
    else:
        verdict = "error"
    row = {
        "job": job,
        "config": cfgname,
        "rep": rep,
        "wall": round(wall, 3),
        "cpu": round(ru.ru_utime + ru.ru_stime, 3),
        "status": rc,
        "verdict": verdict,
        "replay_original": replay_original,
        "replay_original_status": replay_original_status,
        "replay_status": replay_status,
        "seam_inb": seam_inb,
        "replay_no_clobber": replay_no_clobber,
        "replay_mismatch_diag": replay_mismatch_diag,
        "replay": replay,
        "maxrss_mb": round(ru.ru_maxrss / 1024),
        "log": base,
    }
    for k, v in stats.items():
        row.setdefault(k, v)
    row["status"] = rc  # sea-vc's own `status` stat is the same number
    return row


def vlabel(r):
    """A run's verdict for the progress lines: `sat replay=impossible` for a sat that cannot be replayed."""
    return r["verdict"] + (f" replay={r['replay']}" if r.get("replay") else "")


def cmd_run(a):
    vc = os.path.abspath(
        a.sea_vc
        or os.environ.get("SEA_VC")
        or next(
            (
                p
                for p in (
                    REPO + "/build-release/bin/sea-vc",
                    REPO + "/build/bin/sea-vc",
                )
                if os.path.exists(p)
            ),
            "sea-vc",
        )
    )
    if not os.path.exists(vc):
        sys.exit(f"bench: no sea-vc at {vc} (--sea-vc)")
    cd = corpus_dir(a.jobset)
    jobs = sorted(os.path.basename(p)[:-5] for p in glob.glob(cd + "/*.mlir"))
    if a.jobs:
        jobs = [j for j in jobs if j in a.jobs.split(",")]
    if not jobs:
        sys.exit(f"bench: no corpus for {a.jobset}; run `bench.py export {a.jobset}`")
    cfgs = {c: load_yaml("configs", c) for c in a.configs}
    stamp = datetime.datetime.now()
    outdir = os.path.join(
        HOME, "results", stamp.strftime("%Y-%m-%d_%H%M%S") + "-" + (a.name or a.jobset)
    )
    os.makedirs(outdir)
    manifest = {
        "jobset": a.jobset,
        "jobset-contents": load_yaml("jobsets", a.jobset),
        "configs": cfgs,
        "sea-vc": vc,
        "sea-vc-sha256": sha256(vc),
        "sea-mlir": git_state(),
        "host": platform.node(),
        "cpus": os.cpu_count(),
        "date": stamp.isoformat(timespec="seconds"),
        "par": a.par,
        "reps": a.reps,
        "timeout-override": a.timeout,
        "jobs": jobs,
        "command": " ".join(shlex.quote(x) for x in sys.argv),
    }
    yaml.safe_dump(manifest, open(outdir + "/manifest.yaml", "w"), sort_keys=False)
    tasks = [(c, j, r) for c in a.configs for j in jobs for r in range(a.reps)]
    rows = []
    jl = open(outdir + "/rows.jsonl", "w")
    lock = threading.Lock()

    def work(t):
        c, j, r = t
        cfg = cfgs[c]
        row = run_one(
            vc,
            f"{cd}/{j}.mlir",
            cfg,
            c,
            j,
            r,
            outdir,
            a.timeout or cfg.get("timeout", 300),
            cfg.get("mem", 0),
        )
        with lock:
            rows.append(row)
            jl.write(json.dumps(row) + "\n")
            jl.flush()
        print(f"{c} {j} rep={r} {vlabel(row)} {row['wall']}s", flush=True)

    try:
        with cf.ThreadPoolExecutor(a.par) as ex:
            list(ex.map(work, tasks))
    finally:
        write_csv(outdir, rows)
    print(f"out: {outdir}")
    for c in a.configs:
        v = [r["verdict"] for r in rows if r["config"] == c]
        print(
            f"{c}: "
            + " ".join(f"{k}={v.count(k)}" for k in sorted(set(v)))
            + f" wall={sum(r['wall'] for r in rows if r['config'] == c):.1f}"
        )


def write_csv(outdir, rows):
    fixed = ["job", "config", "rep", "wall", "cpu", "status", "verdict"]
    extra = sorted({k for r in rows for k in r} - set(fixed))
    rows = sorted(rows, key=lambda r: (r["config"], r["job"], r["rep"]))
    with open(outdir + "/stats.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(fixed + extra)
        for r in rows:
            w.writerow([r.get(k) for k in fixed + extra])


# ---------------------------------------------------------------- compare
def resolve_dir(d):
    if os.path.isdir(d):
        return d
    rd = os.path.join(HOME, "results")
    if d == "latest":
        c = sorted(glob.glob(rd + "/*"))
        if c:
            return c[-1]
    if os.path.isdir(os.path.join(rd, d)):
        return os.path.join(rd, d)
    c = sorted(glob.glob(rd + "/*" + d + "*"))
    if c:
        return c[-1]
    sys.exit(f"bench: no results dir '{d}'")


def load_rows(d, cfg):
    rows = [json.loads(l) for l in open(d + "/rows.jsonl")]
    cs = sorted({r["config"] for r in rows})
    if cfg is None:
        if len(cs) != 1:
            sys.exit(f"bench: {d} has configs {cs}; pick one with -a/-b")
        cfg = cs[0]
    if cfg not in cs:
        sys.exit(f"bench: {d} has no config '{cfg}' (has {cs})")
    per = {}
    for r in rows:
        if r["config"] == cfg:
            per.setdefault(r["job"], []).append(r)
    return cfg, per


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def cmd_compare(a):
    da = resolve_dir(a.dirA)
    db = resolve_dir(a.dirB) if a.dirB else da
    ca, A = load_rows(da, a.a)
    cb, B = load_rows(db, a.b)
    print(f"A: {da} config={ca}")
    print(f"B: {db} config={cb}")
    common = sorted(set(A) & set(B))
    print(f"jobs: A={len(A)} B={len(B)} common={len(common)}")
    verd = lambda per, j: per[j][0]["verdict"]
    solved = lambda v: v in ("sat", "unsat")
    wall = lambda per, j: statistics.median(r["wall"] for r in per[j])
    dis = [j for j in common if verd(A, j) != verd(B, j)]
    soundness = [j for j in dis if solved(verd(A, j)) and solved(verd(B, j))]
    print(f"disagreements: {len(dis)}")
    for j in soundness:
        print(
            f"DISAGREE-SAT-UNSAT: {j} A={verd(A, j)} B={verd(B, j)}  !!! one side is wrong"
        )
    for j in dis:
        if j not in soundness:
            print(f"disagree: {j} A={verd(A, j)} B={verd(B, j)}")
    sa = [j for j in common if solved(verd(A, j))]
    sb = [j for j in common if solved(verd(B, j))]
    print(f"solved-A: {len(sa)}")
    print(f"solved-B: {len(sb)}")
    both = sorted(set(sa) & set(sb))
    print(f"solved-both: {len(both)}")
    print(f"only-A: {' '.join(sorted(set(sa) - set(sb)))}")
    print(f"only-B: {' '.join(sorted(set(sb) - set(sa)))}")
    ta, tb = sum(wall(A, j) for j in both), sum(wall(B, j) for j in both)
    print(f"time-both-A: {ta:.2f}")
    print(f"time-both-B: {tb:.2f}")
    print(f"time-both-ratio-B/A: {tb / ta:.3f}" if ta else "time-both-ratio-B/A: n/a")
    print(f"time-all-A: {sum(wall(A, j) for j in common):.2f}")
    print(f"time-all-B: {sum(wall(B, j) for j in common):.2f}")
    movers = sorted(common, key=lambda j: -abs(wall(B, j) - wall(A, j)))[: a.top]
    for j in movers:
        x, y = wall(A, j), wall(B, j)
        print(
            f"mover: {j} A={x:.2f} B={y:.2f} diff={y - x:+.2f} ratio={y / x if x else float('inf'):.2f} "
            f"verdicts={verd(A, j)}/{verd(B, j)}"
        )
    # counters (seam.*, vc.*): sums over the jobs both sides solved
    keys = sorted(
        {
            k
            for per in (A, B)
            for j in both
            for k in per[j][0]
            if k.startswith(("seam.", "vc."))
        }
    )
    for k in keys:
        sums = []
        for per in (A, B):
            vs = [num(per[j][0].get(k)) for j in both]
            sums.append(sum(v for v in vs if v is not None))
        if sums[0] != sums[1]:
            print(f"delta: {k} A={sums[0]:g} B={sums[1]:g} diff={sums[1] - sums[0]:+g}")


# ---------------------------------------------------------------- suites (benchmark repos, docs/Benchmarks.md)
WORK = os.path.join(REPO, "bench", "work")
TIERS = ["smoke", "quick", "full", "hard"]
TIER_LIMIT = {
    "smoke": 5,
    "quick": 60,
    "full": 900,
}  # establish: wall time below -> tier
TIER_TIMEOUT = {
    "smoke": 60,
    "quick": 300,
}  # gate: per-job timeout cap by the job's tier
SOLVED = ("sat", "unsat")


class _Flow(list):
    pass


class _Dumper(yaml.SafeDumper):
    pass


_Dumper.add_representer(
    _Flow,
    lambda d, v: d.represent_sequence("tag:yaml.org,2002:seq", v, flow_style=True),
)
MANIFEST_HEADER = """# Benchmark family manifest (sea-mlir utils/bench.py; docs/Benchmarks.md in sea-mlir).
# expected: sat | unsat | unknown-ok | pending (not run yet); tier: smoke (<5 s) | quick (<60 s) | full (<900 s) | hard
# (tracked, not gated); time: wall seconds when established; args: per-job sea-vc args
# appended to the configuration's. counters: {<stat>: {max | min | exact: N, why, seeded}} completeness bounds
# (defaults.counters, counter-rules [{jobs: regex, counters, why}], job counters; a bound without `seeded`
# is deliberate: establish never changes it). premise-class: B | C (absent: A), the --seam premises on the
# job's counterexamples (curated, establish never writes it).
"""


def _flow(x):
    if isinstance(x, dict):
        return {k: _flow(v) for k, v in x.items()}
    if isinstance(x, list) and all(not isinstance(e, (dict, list)) for e in x):
        return _Flow(x)
    return x


def write_manifest(path, m):
    with open(path + ".tmp", "w") as f:
        f.write(MANIFEST_HEADER)
        yaml.dump(_flow(m), f, Dumper=_Dumper, sort_keys=False, width=120)
    os.replace(path + ".tmp", path)


def suite_roots():
    env = os.environ.get("SEA_BENCH_SUITES")
    if env:
        return [(os.path.basename(os.path.normpath(p)), p) for p in env.split(":") if p]
    return (
        [
            (n, os.path.join(WORK, n))
            for n in sorted(os.listdir(WORK))
            if os.path.isdir(os.path.join(WORK, n))
        ]
        if os.path.isdir(WORK)
        else []
    )


def families(sels=None):
    """[(suite, family, dir, manifest)] matching SEL (suite, family or suite/family; none: all)."""
    out = []
    for sn, root in suite_roots():
        for mf in sorted(glob.glob(os.path.join(root, "*", "manifest.yaml"))):
            fam = os.path.basename(os.path.dirname(mf))
            if sels and not any(x in (sn, fam, f"{sn}/{fam}") for x in sels):
                continue
            out.append((sn, fam, os.path.dirname(mf), yaml.safe_load(open(mf))))
    if sels and not out:
        sys.exit(
            f"bench: no family matches {' '.join(sels)} (suites: {[n for n, _ in suite_roots()]})"
        )
    return out


def load_sources():
    src = {}
    for f in ("sources.yaml", "sources.local.yaml"):
        p = os.path.join(REPO, "bench", f)
        if os.path.exists(p):
            src.update((yaml.safe_load(open(p)) or {}).get("sources", {}) or {})
    for k in list(src) + [
        k[len("SEA_BENCH_SOURCE_") :].lower()
        for k in os.environ
        if k.startswith("SEA_BENCH_SOURCE_")
    ]:
        v = os.environ.get("SEA_BENCH_SOURCE_" + k.upper())
        if v is not None:
            src[k] = v
    return src


def cmd_connect(a):
    os.makedirs(WORK, exist_ok=True)
    for name, url in sorted(load_sources().items()):
        dst = os.path.join(WORK, name)
        if not url:
            print(
                f"source: {name} skipped (no url; set it in bench/sources.local.yaml or SEA_BENCH_SOURCE_{name.upper()})"
            )
            continue
        local = os.path.expanduser(url)
        if os.path.isdir(local):
            if os.path.islink(dst) or not os.path.exists(dst):
                if os.path.islink(dst):
                    os.unlink(dst)
                os.symlink(os.path.abspath(local), dst)
                print(f"source: {name} linked {dst} -> {os.path.abspath(local)}")
            else:
                print(f"source: {name} NOT linked: {dst} exists and is not a symlink")
            continue
        if os.path.islink(dst):
            os.unlink(dst)
        if os.path.isdir(os.path.join(dst, ".git")):
            p = subprocess.run(
                ["git", "-C", dst, "pull", "--ff-only", "-q"],
                capture_output=True,
                text=True,
            )
            what = "pulled"
        else:
            p = subprocess.run(
                ["git", "clone", "-q", url, dst], capture_output=True, text=True
            )
            what = "cloned"
        print(
            f"source: {name} {what if p.returncode == 0 else 'FAILED (' + p.stderr.strip()[-200:] + ')'} {url}"
        )
    for sn, root in suite_roots():
        print(f"suite: {sn} {root} commit={suite_commit(root)}")


def suite_commit(root):
    p = subprocess.run(
        ["git", "-C", root, "rev-parse", "--short=12", "HEAD"],
        capture_output=True,
        text=True,
    )
    d = subprocess.run(
        ["git", "--no-optional-locks", "-C", root, "status", "--porcelain"],
        capture_output=True,
        text=True,
    ).stdout.strip()
    return (p.stdout.strip() or "none") + ("-dirty" if d else "")


def cmd_suites(a):
    for sn, root in suite_roots():
        print(f"suite: {sn} {os.path.realpath(root)} commit={suite_commit(root)}")
    for sn, fam, _d, m in families(a.sel):
        jobs = m.get("jobs") or {}
        tiers = {t: sum(1 for j in jobs.values() if j.get("tier") == t) for t in TIERS}
        exp = {}
        for j in jobs.values():
            exp[j.get("expected") or "none"] = (
                exp.get(j.get("expected") or "none", 0) + 1
            )
        print(
            f"family: {sn}/{fam} jobs={len(jobs)} "
            + " ".join(f"{t}={n}" for t, n in tiers.items())
            + " "
            + " ".join(f"{k}={v}" for k, v in sorted(exp.items()))
            + f" args={' '.join((m.get('defaults') or {}).get('args', []))} license={m.get('license', '?')!r}"
        )


def job_counters(m, j, v):
    """The effective counter bounds of job J (entry V) of manifest M: {key: {max/min/exact, why, seeded}}.
    defaults.counters, then every counter-rules entry whose `jobs` regex matches J, then the job's own; a
    null value removes the inherited bound."""
    out = {}

    def merge(d, why=None):
        for k, b in (d or {}).items():
            if b is None:
                out.pop(k, None)
                continue
            b = dict(b) if isinstance(b, dict) else {"exact": b}
            if why and "why" not in b:
                b["why"] = why
            out[k] = b

    merge((m.get("defaults") or {}).get("counters"))
    for r in m.get("counter-rules") or []:
        if re.search(r["jobs"], j):
            merge(r.get("counters"), r.get("why"))
    merge(v.get("counters"))
    return out


def observed_counter(row, key):
    """KEY's value in a run: its BRUNCH_STAT, else a `key: value` line of the run's stdout, else (KEY =
    <line>.<field>) the `field=value` entry of a `<line>: k=v k=v ...` line (`vc.box-all.expanded`:
    the lowering's form); None if absent or not a number."""
    x = row.get(key)
    if x is None and row.get("log"):
        try:
            text = as_text(open(row["log"] + ".out").read())
        except OSError:
            text = ""
        mm = re.search(r"^" + re.escape(key) + r": (\S+)", text, re.M)
        if mm:
            x = mm.group(1)
        elif "." in key:
            line, field = key.rsplit(".", 1)
            mm = re.search(r"^" + re.escape(line) + r": (.*)$", text, re.M)
            for kv in mm.group(1).split() if mm else []:
                k, eq, val = kv.partition("=")
                if eq and k == field:
                    x = val
                    break
    return num(x)


def counter_breaks(bound, val):
    """'' when VAL meets BOUND, else what it breaks."""
    if val is None:
        return "missing"
    for k, ok in (
        ("exact", lambda b: val == b),
        ("max", lambda b: val <= b),
        ("min", lambda b: val >= b),
    ):
        if k in bound and not ok(float(bound[k])):
            return (
                f"{k}={bound[k]:g}"
                if isinstance(bound[k], float)
                else f"{k}={bound[k]}"
            )
    return ""


def fmt_num(x):
    return "missing" if x is None else f"{x:g}"


def gate_config(name, m):
    """(args, solver, timeout, mem) of config NAME for family manifest M."""
    d = m.get("defaults") or {}
    base = dict(
        args=list(d.get("args", [])),
        solver=None,
        timeout=d.get("timeout", 900),
        mem=d.get("mem", 0),
    )
    if name == "default":
        return base
    if name.startswith("default+"):
        c = load_yaml("configs", name[len("default+") :])
        base["args"] += c.get("args", [])
        base["solver"] = c.get("solver")
        return base
    c = load_yaml("configs", name)
    return dict(
        args=c.get("args", []),
        solver=c.get("solver"),
        timeout=c.get("timeout", base["timeout"]),
        mem=c.get("mem", base["mem"]),
    )


def suite_tasks(sels, tiers, jobs_re, tag=None):
    out = []
    for sn, fam, d, m in families(sels):
        for j, v in (m.get("jobs") or {}).items():
            if tag and tag not in (v.get("tags") or []):
                continue
            if v.get("tier") in tiers and (
                not jobs_re or re.search(jobs_re, f"{sn}/{fam}/{j}")
            ):
                out.append((sn, fam, d, m, j, v))
    return out


def check_replay_fields(tasks):
    """The manifest field `replay` takes one value, `impossible`: anything else is an error, not a default."""
    bad = [
        f"{sn}/{fam}/{j}: replay: {v['replay']!r}"
        for sn, fam, _, _, j, v in tasks
        if v.get("replay") not in (None, "impossible")
    ]
    if bad:
        sys.exit(
            "bench: manifest field `replay` must be `impossible`: " + "; ".join(bad)
        )


PREMISE_CLASSES = ("B", "C")
PREMISE_DOC = "docs/Benchmarks.md, premise classes"


def check_premise_fields(tasks):
    """`premise-class` is B or C (absent: class A); class B requires a sat, so not on an unsat job."""
    bad = []
    for sn, fam, _, _, j, v in tasks:
        pc = v.get("premise-class")
        if pc is not None and pc not in PREMISE_CLASSES:
            bad.append(f"{sn}/{fam}/{j}: premise-class: {pc!r} (B or C)")
        elif pc == "B" and v.get("expected") == "unsat":
            bad.append(
                f"{sn}/{fam}/{j}: premise-class: B on an expected unsat job (class B requires a sat)"
            )
    if bad:
        sys.exit("bench: manifest field `premise-class`: " + "; ".join(bad))


def premise_signal(r):
    """What a run shows of the --seam premises on its counterexample: `left-object` (a sat confirmed by
    the replay whose original-program run left its object: replay.original status=memory-error),
    `unconfirmed` (rc 2: the replay reached the failing assertion, the values differ, and the A-INB
    fallback check ran: found or undetermined; the vc.replay-mismatch error, not vc.replay-no-failure),
    `inb-violation` (rc 4: the check found an access outside an object, verdict inb-violation, or a run that
    breaks a GEP promise the stride rule trusts, verdict promise-broken), else None. A run whose replay
    reports a violated Mem-SSA no-clobber claim (`replay: no-clobber-violated:`) shows none."""
    if r.get("replay_no_clobber"):
        return None
    if r["verdict"] == "sat" and r.get("replay_original_status") == "memory-error":
        return "left-object"
    if (
        r["verdict"] == "replay-mismatch"
        and r.get("replay_status") == "assert-failed"
        and r.get("seam_inb")
        and r.get("replay_mismatch_diag")
    ):
        return "unconfirmed"
    if r["verdict"] in ("inb-violation", "promise-broken"):
        return "inb-violation"
    return None


def premise_hint(r, sig):
    """The hint for a run of an undeclared job (class A) that shows a premise signal."""
    what = {
        "left-object": "replay.original status=memory-error",
        "unconfirmed": f"replay: assert-failed, seam.inb-violation: {r.get('seam_inb')}",
        "inb-violation": "vc.ack-stride-promise-broken (rc 4)"
        if r["verdict"] == "promise-broken"
        else "seam.inb-violation (rc 4)",
    }[sig]
    return f"{what}: premise may fail on this job: declare it (premise-class, {PREMISE_DOC})"


def mem_available_gb():
    try:
        for line in open("/proc/meminfo"):
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / (1 << 20)
    except OSError:
        pass
    return float("inf")


GUARD = threading.Lock()


def mem_total_gb():
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1 << 30)
    except (ValueError, OSError):
        return float("inf")


def wait_for_machine(a):
    """Shared machine: start a job only with >= --min-free-gb available (at most a quarter of the
    machine's memory, so small machines do not wait forever) and load <= --max-load."""
    need = min(a.min_free_gb, mem_total_gb() / 4)
    with GUARD:
        warned = False
        while mem_available_gb() < need or os.getloadavg()[0] > a.max_load:
            if not warned:
                print(
                    f"wait: available={mem_available_gb():.0f}GB load={os.getloadavg()[0]:.1f}",
                    flush=True,
                )
                warned = True
            time.sleep(15)
        time.sleep(1)  # stagger starts


def run_suite(
    tasks, cfgnames, vc, a, outdir, tier_cap, done=None, prior=(), extra_args=()
):
    rows = list(prior)
    lock = threading.Lock()
    jl = open(outdir + "/rows.jsonl", "w")
    for r in prior:
        jl.write(json.dumps(r) + "\n")
    have = {(r["job"], r["config"]) for r in prior}

    def work(t):
        (sn, fam, d, m, j, v), c = t
        if (f"{sn}.{fam}.{j}", c) in have:
            return
        cfg = gate_config(c, m)
        cfg = dict(cfg, args=cfg["args"] + list(v.get("args") or []) + list(extra_args))
        if a.mem:
            cfg["mem"] = a.mem
        wait_for_machine(a)
        to = cfg["timeout"]
        if tier_cap and v.get("tier") in TIER_TIMEOUT:
            to = min(to, TIER_TIMEOUT[v["tier"]])
        row = run_one(
            vc,
            os.path.join(d, j + ".mlir"),
            cfg,
            c,
            f"{sn}.{fam}.{j}",
            0,
            outdir,
            to,
            cfg["mem"],
        )
        row.update(
            suite=sn,
            family=fam,
            name=j,
            expected=v.get("expected"),
            tier=v.get("tier"),
            recorded=v.get("time"),
            timeout=to,
            counters=job_counters(m, j, v),
            replay_expected=v.get("replay"),
            premise_class=v.get("premise-class"),
        )
        with lock:
            rows.append(row)
            jl.write(json.dumps(row) + "\n")
            jl.flush()
            if done:
                done(row)

    with cf.ThreadPoolExecutor(a.par) as ex:
        list(ex.map(work, [(t, c) for c in cfgnames for t in tasks]))
    jl.close()
    return rows


def find_vc(a):
    vc = os.path.abspath(
        a.sea_vc
        or os.environ.get("SEA_VC")
        or next(
            (
                p
                for p in (
                    REPO + "/build-release/bin/sea-vc",
                    REPO + "/build/bin/sea-vc",
                )
                if os.path.exists(p)
            ),
            "sea-vc",
        )
    )
    if not os.path.exists(vc):
        sys.exit(f"bench: no sea-vc at {vc} (--sea-vc)")
    return vc


def vc_has_option(vc, name):
    """Does sea-vc VC know --NAME (its --help-hidden lists it)?"""
    try:
        out = subprocess.run(
            [vc, "--help-hidden"], capture_output=True, text=True, timeout=60
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return re.search(rf"--{re.escape(name)}\b", out) is not None


def cmd_gate(a):
    if a.tier not in TIERS:
        sys.exit(f"bench: tier must be one of {TIERS}")
    vc = find_vc(a)
    tiers = TIERS[: TIERS.index(a.tier) + 1]
    cfgs = [c for c in a.config.split(",") if c]
    tasks = suite_tasks(a.suite, tiers, a.jobs, a.tag)
    check_replay_fields(tasks)
    check_premise_fields(tasks)
    if not tasks:
        sys.exit(
            "bench: no jobs (connect suites with `bench.py connect`, or check the tier / --suite / --jobs)"
        )
    stamp = datetime.datetime.now()
    outdir = os.path.join(
        HOME, "results", stamp.strftime("%Y-%m-%d_%H%M%S") + f"-gate-{a.tier}"
    )
    os.makedirs(outdir)
    gs = git_state()
    print(
        f"gate: {a.tier} configs={','.join(cfgs)} jobs={len(tasks)} runs={len(tasks) * len(cfgs)} par={a.par}"
    )
    print(f"sea-vc: {vc}")
    # a sat is also replayed on the program before the lowering (sea-vc --replay-original);
    # a sea-vc without the option (an older base of bench-perf) runs without it
    replay_orig = not a.no_replay_original and vc_has_option(vc, "replay-original")
    print(
        "replay-original: "
        + (
            "on"
            if replay_orig
            else "off"
            if a.no_replay_original
            else "off (unsupported by sea-vc)"
        )
    )
    print(f"out: {outdir}", flush=True)

    def done(r):
        if a.verbose:
            print(f"run: {r['config']} {r['job']} {vlabel(r)} {r['wall']}s", flush=True)

    t0 = time.monotonic()
    rows = run_suite(
        tasks,
        cfgs,
        vc,
        a,
        outdir,
        True,
        done,
        extra_args=["--replay-original"] if replay_orig else [],
    )
    write_csv(outdir, rows)
    fails, slow, solved, cfails, replayed = [], [], [], [], []
    # premise classes (docs/Benchmarks.md): outcomes of declared jobs, hints on undeclared ones
    premise = {"left-object": [], "unconfirmed": [], "inb-violation": [], "unsat-c": []}
    hints = []
    nbounds = 0
    by_job = {}
    for r in sorted(rows, key=lambda r: (r["job"], r["config"])):
        by_job.setdefault(r["job"], []).append(r)
        exp, v, tier = r["expected"], r["verdict"], r["tier"]
        key = f"{r['job']} config={r['config']}"
        pc, sig = r.get("premise_class"), premise_signal(r)
        nfails = len(fails)
        if pc and sig in ("unconfirmed", "inb-violation"):
            # sat whose replay failed in a way a premise violation explains: a pass for B and C
            rerun = (
                f"python3 utils/bench.py gate {tier} --jobs '^{r['suite']}/{r['family']}/{r['name']}$'"
                + (f" --config {r['config']}" if r["config"] != "default" else "")
                + " --no-record"
            )
            premise[sig].append(f"{key} class={pc} log={r['log']}.out rerun: {rerun}")
        elif pc == "B" and v == "unsat":
            fails.append(
                f"WRONG {key} verdict=unsat expected={exp}"
                " (premise-class B: a counterexample that keeps the premises exists)"
            )
        elif v in ("replay-mismatch", "lowering-mismatch"):
            fails.append(f"{v} {key} expected={exp}")
        elif pc == "C" and v == "sat" and not sig:
            # class C: every counterexample violates the premises; a clean confirmed sat refutes that
            fails.append(
                f"misclassified {key} verdict=sat expected={exp} (premise-class C, but the sat replayed"
                " clean with no premise signal: declare B)"
            )
        elif exp in SOLVED and v != exp and not (pc == "C" and v in SOLVED):
            # class C: unsat, or a sat with a premise signal (above)
            fails.append(
                f"{'WRONG' if v in SOLVED else 'unsolved'} {key} verdict={v} expected={exp}"
            )
        elif (
            r.get("replay") == "impossible" and r.get("replay_expected") != "impossible"
        ):
            fails.append(
                f"replay-impossible {key} verdict={v} expected={exp}"
                " (the sat model cannot be replayed; manifest `replay: impossible` allows it)"
            )
        elif exp == "unknown-ok" and v in SOLVED:
            solved.append(f"{key} verdict={v}")
        elif (
            exp == "unknown-ok"
            and v
            in (
                "error",
                "inb-violation",
                "premise-failed",
                "promise-broken",
                "original-memory-error",
                "check-invalid",
                "check-unproven",
            )
            and tier != "hard"
        ):
            fails.append(f"{v} {key} expected={exp}")
        if pc and sig == "left-object":
            premise["left-object"].append(key)
        if pc == "C" and v == "unsat":
            premise["unsat-c"].append(key)
        if not pc and sig:
            if len(fails) > nfails:  # the failure line names the cause
                fails[-1] += " -- " + premise_hint(r, sig)
            else:
                hints.append(f"{key} {premise_hint(r, sig)}")
        # completeness: counter bounds, for a run that answered as expected (a wrong or missing verdict is
        # reported above); the default configuration only (other options change the counts)
        if (
            not a.no_counters
            and r["config"] == "default"
            and v in SOLVED
            and (exp not in SOLVED or v == exp)
        ):
            for ck, b in sorted((r.get("counters") or {}).items()):
                nbounds += 1
                val = observed_counter(r, ck)
                br = counter_breaks(b, val)
                if br:
                    cfails.append(
                        f"{key} {ck}={fmt_num(val)} bound {br}"
                        + (
                            f" why: {b['why'][:80]}..."
                            if len(b.get("why") or "") > 80
                            else f" why: {b['why']}"
                            if b.get("why")
                            else ""
                        )
                        + (" (seeded)" if "seeded" in b else "")
                    )
        if (
            r.get("replay_expected") == "impossible"
            and v == "sat"
            and r.get("replay") != "impossible"
        ):
            replayed.append(f"{key} verdict=sat (manifest replay: impossible)")
        if (
            r["recorded"]
            and v in SOLVED
            and r["wall"] > a.slow * r["recorded"]
            and r["wall"] - r["recorded"] > 1
        ):
            slow.append(
                f"{key} wall={r['wall']} recorded={r['recorded']} ratio={r['wall'] / r['recorded']:.2f}"
            )
    for j, rs in by_job.items():
        vs = {r["verdict"] for r in rs if r["verdict"] in SOLVED}
        # class C allows both verdicts when every sat run shows a premise signal
        exempt = rs[0].get("premise_class") == "C" and all(
            premise_signal(r) for r in rs if r["verdict"] == "sat"
        )
        if len(vs) > 1 and not exempt:
            fails.append(
                f"DISAGREE-SAT-UNSAT {j} "
                + " ".join(f"{r['config']}={r['verdict']}" for r in rs)
            )
    ok = not fails and not cfails
    for f in fails:
        print(f"FAIL: {f}")
    for f in cfails:
        print(f"FAIL: counter {f}")
    for s_ in slow:
        print(f"slow: {s_}")
    for s_ in solved:
        print(f"solved: {s_}")
    for s_ in replayed:
        print(f"replayed: {s_}")
    for s_ in premise["unconfirmed"]:
        print(f"premise-unconfirmed: {s_}")
    for s_ in premise["inb-violation"]:
        print(f"premise-inb-violation: {s_}")
    for s_ in hints:
        print(f"premise-hint: {s_}")
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    wall = time.monotonic() - t0
    print("verdicts: " + " ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    if replay_orig:
        ro = [r.get("replay_original") for r in rows]
        print(
            f"replay-original: match={ro.count('yes')} undetermined={ro.count('undetermined')}"
            f" mismatch={ro.count('no')} skipped={ro.count('skipped')}"
        )
    ri = [r.get("replay_expected") for r in rows if r.get("replay") == "impossible"]
    print(f"replay-impossible: {len(ri)} allowed={ri.count('impossible')}")
    pruns = sum(1 for r in rows if r.get("premise_class"))
    print(
        f"premises: declared-runs={pruns}"
        + "".join(f" {k}={len(x)}" for k, x in premise.items())
        + f" hints={len(hints)}"
    )
    print(f"failures: {len(fails)}")
    print(f"counter-bounds: {nbounds}")
    print(f"counter-failures: {len(cfails)}")
    print(f"slow: {len(slow)}")
    print(f"wall: {wall:.0f}")
    print(f"gate: {'pass' if ok else 'fail'}")
    rec = {
        "date": stamp.isoformat(timespec="seconds"),
        "tier": a.tier,
        "configs": cfgs,
        "result": "pass" if ok else "fail",
        "sea-mlir": gs,
        "sea-vc-sha256": sha256(vc),
        "host": platform.node(),
        "par": a.par,
        "suites": {sn: suite_commit(root) for sn, root in suite_roots()},
        "selection": {
            "suite": a.suite or "all",
            "jobs": a.jobs or "all",
            "tag": a.tag or "all",
        },
        "jobs": len(tasks),
        "runs": len(rows),
        "wall": round(wall),
        "verdicts": counts,
        "failures": fails,
        "counter-bounds": nbounds,
        "counter-failures": cfails,
        "slow": slow,
        "solved": solved,
        "replay-impossible": len(ri),
        "replayed": replayed,
        "premises": dict(
            {"declared-runs": pruns}, **{k: len(x) for k, x in premise.items()}
        ),
        "premise-unconfirmed": premise["unconfirmed"] + premise["inb-violation"],
        "premise-hints": hints,
        "results": outdir,
        "command": " ".join(shlex.quote(x) for x in sys.argv),
    }
    if not a.no_record:
        gates_dir = os.path.join(HOME, "gates")
        os.makedirs(gates_dir, exist_ok=True)
        path = os.path.join(gates_dir, f"{stamp:%Y-%m-%d}-{gs['rev'][:12]}.yaml")
        old = (
            (yaml.safe_load(open(path)) or {}).get("gates", [])
            if os.path.exists(path)
            else []
        )
        with open(path, "w") as f:
            f.write(
                "# bench.py gate records (docs/Benchmarks.md); one entry per gate run at this commit\n"
            )
            yaml.dump(
                {"gates": old + [_flow(rec)]},
                f,
                Dumper=_Dumper,
                sort_keys=False,
                width=120,
            )
        print(f"record: {path}")
    return 0 if ok else 1


def record_counters(keys, m, label, j, v, r, stamp):
    """establish --record-counters: seed {max: observed} for each KEY without a deliberate bound."""
    eff = job_counters(m, j, v)
    for k in keys:
        val = observed_counter(r, k)
        b = eff.get(k)
        if b is not None and "seeded" not in b:  # deliberate: never changed
            br = counter_breaks(b, val)
            if br:
                print(f"COUNTER-OVER: {label} {k}={fmt_num(val)} bound {br}")
            continue
        if val is None:
            print(f"counter-missing: {label} {k}")
            continue
        v.setdefault("counters", {})[k] = {
            "max": int(val) if val == int(val) else val,
            "seeded": stamp,
        }


def cmd_establish(a):
    vc = find_vc(a)
    tasks = suite_tasks(a.sel, TIERS + [None], a.jobs)
    check_replay_fields(tasks)
    check_premise_fields(tasks)
    stamp = datetime.datetime.now()
    outdir = os.path.join(
        HOME, "results", stamp.strftime("%Y-%m-%d_%H%M%S") + "-establish"
    )
    os.makedirs(outdir)
    print(f"establish: jobs={len(tasks)} par={a.par} out={outdir}", flush=True)
    prior = []
    if a.resume:  # reuse the finished runs of an interrupted establish
        names = {f"{t[0]}.{t[1]}.{t[4]}" for t in tasks}
        prior = [
            r
            for r in (
                json.loads(l)
                for l in open(os.path.join(resolve_dir(a.resume), "rows.jsonl"))
            )
            if r["job"] in names and r["config"] == "default"
        ]
        print(f"resume: {len(prior)} runs from {a.resume}", flush=True)
    if a.no_run:  # write what --resume has; jobs without a run become `pending`
        rows = prior
        with open(outdir + "/rows.jsonl", "w") as jl:
            jl.writelines(json.dumps(r) + "\n" for r in rows)
    else:
        rows = run_suite(
            tasks,
            ["default"],
            vc,
            a,
            outdir,
            False,
            lambda r: print(f"run: {r['job']} {vlabel(r)} {r['wall']}s", flush=True),
            prior,
        )
    write_csv(outdir, rows)
    est = {
        "sea-mlir": git_state(),
        "sea-vc-sha256": sha256(vc),
        "date": stamp.isoformat(timespec="seconds"),
        "host": platform.node(),
        "par": a.par,
        "results": outdir,
    }
    by = {}
    for r in rows:
        by.setdefault((r["suite"], r["family"]), {})[r["name"]] = r
    for sn, fam, d, m in families(a.sel):
        rs = by.get((sn, fam), {})
        sel = {t[4] for t in tasks if t[0] == sn and t[1] == fam}
        for j in sel - set(rs):
            if m["jobs"][j].get("expected") in (None, "pending"):
                m["jobs"][j].update(expected="pending", tier=None, time=None)
                print(f"pending: {sn}/{fam}/{j}")
        if not rs:
            write_manifest(os.path.join(d, "manifest.yaml"), m)
            continue
        for j, r in rs.items():
            v = m["jobs"][j]
            if a.record_counters and r["verdict"] in SOLVED:
                record_counters(
                    a.record_counters.split(","),
                    m,
                    f"{sn}/{fam}/{j}",
                    j,
                    v,
                    r,
                    f"{stamp:%Y-%m-%d} sea-vc {est['sea-vc-sha256'][:12]}",
                )
            if r.get("replay") == "impossible" and v.get("replay") != "impossible":
                # deliberate: establish never writes `replay`; the gate fails until a curator does
                print(
                    f"replay-impossible: {sn}/{fam}/{j} (sat; the gate fails without `replay: impossible`)"
                )
            sig = premise_signal(r)
            if sig and not v.get("premise-class"):
                # deliberate: establish never writes `premise-class` (curation)
                print(f"premise-hint: {sn}/{fam}/{j} {premise_hint(r, sig)}")
            if a.counters_only:
                continue
            verdict = r["verdict"]
            if v.get("premise-class") and sig in ("unconfirmed", "inb-violation"):
                verdict = "sat"  # the gate passes it as a sat of a declared job
            if v.get("premise-class") == "C" and verdict == "sat" and not sig:
                print(
                    f"PREMISE-C-MISCLASSIFIED: {sn}/{fam}/{j} (a clean confirmed sat on premise-class C: declare B)"
                )
            if v.get("premise-class") == "B" and verdict == "unsat":
                # class B requires a sat: keep the manifest's verdict, the gate fails the job
                print(
                    f"PREMISE-B-UNSAT: {sn}/{fam}/{j} (premise-class B requires a sat; expected left as is)"
                )
                continue
            if verdict in SOLVED:
                v["expected"] = verdict
                v["tier"] = next(
                    t for t in TIERS if t == "hard" or r["wall"] < TIER_LIMIT[t]
                )
            else:
                v["expected"], v["tier"] = "unknown-ok", "hard"
            v["time"] = round(r["wall"], 2)
            v["established"] = r["verdict"]
            he = v.get("harness-expected")
            if he and r["verdict"] in SOLVED and he != r["verdict"]:
                print(
                    f"HARNESS-DISAGREE: {sn}/{fam}/{j} verdict={r['verdict']} harness-expected={he}"
                )
        if not a.counters_only:
            m["established"] = dict(est, config=gate_config("default", m))
        write_manifest(os.path.join(d, "manifest.yaml"), m)
        print(f"manifest: {d}/manifest.yaml")
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    print("verdicts: " + " ".join(f"{k}={v}" for k, v in sorted(counts.items())))


def main():
    if "--agent" in sys.argv:
        print(AGENT, end="")
        return 0
    p = argparse.ArgumentParser(
        description="sea-vc benchmark tool (--agent for the guide)"
    )
    sp = p.add_subparsers(dest="cmd", required=True)
    sp.add_parser("list").set_defaults(f=cmd_list)
    e = sp.add_parser("export")
    e.add_argument("jobset")
    e.add_argument("--par", type=int, default=16)
    e.add_argument("--force", action="store_true")
    e.set_defaults(f=cmd_export)
    r = sp.add_parser("run")
    r.add_argument("jobset")
    r.add_argument("configs", nargs="+")
    r.add_argument("--par", type=int, default=4)
    r.add_argument("--timeout", type=int)
    r.add_argument("--reps", type=int, default=1)
    r.add_argument("--sea-vc")
    r.add_argument("--name")
    r.add_argument("--jobs", help="comma-separated subset")
    r.set_defaults(f=cmd_run)
    c = sp.add_parser("compare")
    c.add_argument("dirA")
    c.add_argument("dirB", nargs="?")
    c.add_argument("-a", help="config in A")
    c.add_argument("-b", help="config in B")
    c.add_argument("--top", type=int, default=10)
    c.set_defaults(f=cmd_compare)
    sp.add_parser("connect").set_defaults(f=cmd_connect)
    s = sp.add_parser("suites")
    s.add_argument("sel", nargs="*")
    s.set_defaults(f=cmd_suites)
    g = sp.add_parser("gate")
    g.add_argument("tier")
    g.add_argument("--config", default="default")
    g.add_argument(
        "--suite", action="append", help="suite, family or suite/family (repeatable)"
    )
    g.add_argument("--par", type=int, default=4)
    g.add_argument("--sea-vc")
    g.add_argument(
        "--mem",
        type=int,
        help="memory cap in MB for every job (overrides the manifests)",
    )
    g.add_argument(
        "--min-free-gb",
        type=float,
        default=60,
        help="start a job only with this much memory available",
    )
    g.add_argument(
        "--max-load",
        type=float,
        default=32,
        help="start a job only below this load average",
    )
    g.add_argument("--slow", type=float, default=2.0)
    g.add_argument("--jobs", help="regex over suite/family/job")
    g.add_argument(
        "--no-record",
        action="store_true",
        help="write no gate record (results and logs are kept)",
    )
    g.add_argument(
        "--no-counters", action="store_true", help="do not check the counter bounds"
    )
    g.add_argument(
        "--tag", help="only jobs with this tag (canary: the small cross-family gate)"
    )
    g.add_argument(
        "--no-replay-original",
        action="store_true",
        help="run without sea-vc --replay-original",
    )
    g.add_argument("-v", "--verbose", action="store_true")
    g.set_defaults(f=cmd_gate)
    e = sp.add_parser("establish")
    e.add_argument("sel", nargs="+")
    e.add_argument("--par", type=int, default=4)
    e.add_argument(
        "--resume", help="results dir of an interrupted establish: reuse its runs"
    )
    e.add_argument(
        "--no-run",
        action="store_true",
        help="with --resume: only write the manifests (unrun jobs: pending)",
    )
    e.add_argument("--sea-vc")
    e.add_argument(
        "--mem",
        type=int,
        help="memory cap in MB for every job (overrides the manifests)",
    )
    e.add_argument(
        "--min-free-gb",
        type=float,
        default=60,
        help="start a job only with this much memory available",
    )
    e.add_argument(
        "--max-load",
        type=float,
        default=32,
        help="start a job only below this load average",
    )
    e.add_argument("--jobs", help="regex over suite/family/job")
    e.add_argument(
        "--record-counters",
        help="comma-separated counters: seed {max: observed} bounds",
    )
    e.add_argument(
        "--counters-only",
        action="store_true",
        help="with --record-counters: leave expected / tier / time / established as they are",
    )
    e.set_defaults(f=cmd_establish)
    a = p.parse_args()
    return a.f(a) or 0


if __name__ == "__main__":
    sys.exit(main())
