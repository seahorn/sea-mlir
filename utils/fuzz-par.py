#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Run several `sea-run --fuzz` processes on one program (fuzz-par.py --agent).

The fuzzer is single-threaded; this runs N processes with distinct seeds,
each with its own --fuzz-out directory and --fuzz-coverage-out file, merges
their fuzz.progress lines, reports the first failure and the coverage of
each process and of their union. With --sync, each process also reads the
new-coverage files of the others (sea-run --fuzz-sync).
"""

import argparse
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time

AGENT = """\
fuzz-par: run N `sea-run --fuzz` processes on one program, distinct seeds.
WHY: the fuzzer is single-threaded; N processes find a failure in the least
  time of N independent runs, and their coverage joins.
USAGE:
  utils/fuzz-par.py -n N --out DIR [--seed-base B] [--sea-run PATH]
      [--progress S] [--wait-all] [--sync [--sync-period S]]
      -- <program.mlir> <sea-run args>...
  The args after -- go to every process (give --fuzz=<n> and/or
  --fuzz-time=<s>). Not allowed there (the launcher sets them): --seed,
  --fuzz-out, --fuzz-coverage-out, --fuzz-progress, --format.
  Process i (0..N-1) runs with --seed=B+i (B default 1),
  --fuzz-out=DIR/p<i>/out, --fuzz-coverage-out=DIR/p<i>/cov.txt; its
  stdout and stderr go to DIR/p<i>/stdout.txt, stderr.txt. DIR must be new
  or empty.
  --progress S: each process prints fuzz.progress every S seconds and the
  launcher prints a merged line every S seconds (default 0: none).
  At the first failure (fuzz.failure or fuzz.memory-safety line) the other
  processes are stopped (SIGTERM), unless --wait-all. sea-run prints that
  line at its end: with --keep-going passed through, the first failure is
  known only when a process ends (time= still says when it was found).
  --sync: process i also gets --fuzz-sync=<the out dirs of the others>:
  every --sync-period S seconds (sea-run's --fuzz-sync-period, default 5,
  not tuned) it runs their new cov-*.txt files as trials, and keeps those
  that add coverage by its own --fuzz-keep rule. Not reproducible: what a
  process imports depends on timing. Without --sync the processes are
  independent (same seeds = same runs).
OUTPUT (key: value lines, stdout):
  fuzz-par.progress: <wall s> reported=<k>/<N> trials-sum=<n> steps-sum=<n>
      failures-sum=<n> blocks-max=<n> edges-max=<n> paths-max=<n>
      assumes-max=<n> frontier-max=<n>
      (the latest fuzz.progress of each process that has printed one; sums
       of the cost, max of the coverage: the union needs the ids, at the end)
  fuzz-par.process: <i> seed=<s> exit=<rc> end=done|failure|stopped|error
      from=final|progress|none trials=<n> steps=<n> blocks=<n> edges=<n>
      paths=<n> assumes=<n> corpus=<n> [imported=<n> sync-kept=<n>
      sync-skipped=<n>]
      (from: the process's fuzz.trials / fuzz.coverage lines, or its last
       fuzz.progress if it was stopped, or nothing; stopped: by the launcher;
       corpus: its corpus entries; imported, sync-kept, sync-skipped: with
       --sync, its fuzz.sync line, - if it printed none)
  fuzz-par.first-failure: process=<i> seed=<s> kind=failure|memory-safety
      trial=<n> time=<s> wall=<s> at=<where> [file=<path>]
      (time: the process's own clock from its first trial; wall: the
       launcher's, from the start, when the line arrived; or `none`)
  fuzz-par.union: blocks=<n> blocks-total=<n> edges=<n> assumes=<n>
      assumes-total=<n> paths=<n> [paths-capped=1] processes=<k>/<N>
      (union of the coverage files of the processes that ended on their
       own; stopped processes write none)
  fuzz-par.model: frontier-max=<n> closed=<n> closed-gates=<id,...>|-
      [cmp-buckets=<hit>/<total> closed-distance=<id>:<d>,...|-]
      (the joined model coverage of the same files: the deepest assume
       frontier of any trial; the sea.assume gates that blocked trials and
       that no process passed; unless --fuzz-cmp=false the comparison
       buckets of all processes and the least branch distance of each
       closed gate; none when the program has no sea.assume /
       sea.unreachable)
  fuzz-par.summary: processes=<N> wall=<s> failures=<processes with one>
      sync=on|off
EXIT: 0 ran; 4 a process found a memory-safety finding (as sea-run);
  1 bad options, or a process failed (exit other than 0 / 4, not stopped
  by the launcher: fuzz-par.error line).
"""

RESERVED = (
    "--seed",
    "--fuzz-out",
    "--fuzz-coverage-out",
    "--fuzz-progress",
    "--format",
)

SYNC_RESERVED = ("--fuzz-sync", "--fuzz-sync-period")

KV = re.compile(r"(\S+)=(\S+)")


def fields(rest):
    """The k=v fields of a line's rest (after `key: value`)."""
    return dict(KV.findall(rest))


class Proc:
    """One sea-run process and what it printed."""

    def __init__(self, i, seed, cmd, pdir):
        self.i, self.seed, self.dir = i, seed, pdir
        self.cov = os.path.join(pdir, "cov.txt")
        self.out = open(os.path.join(pdir, "stdout.txt"), "w")
        self.err = open(os.path.join(pdir, "stderr.txt"), "w")
        self.popen = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=self.err, text=True, bufsize=1
        )
        self.errored = False
        self.progress = None  # fields of the latest fuzz.progress
        self.trials = None  # fields of fuzz.trials
        self.coverage = None  # fields of fuzz.coverage
        self.corpus = None  # fields of fuzz.corpus
        self.sync = None  # fields of fuzz.sync
        self.failed = False
        self.stopped = False


def reader(p, events):
    """Copies p's stdout to its file; sends (process, line, wall) events."""
    for line in p.popen.stdout:
        p.out.write(line)
        events.put((p, line.rstrip("\n"), time.monotonic()))
    p.out.close()
    events.put((p, None, time.monotonic()))


def merged(procs, t):
    """The fuzz-par.progress line."""
    rep = [p.progress for p in procs if p.progress]
    s = {k: sum(int(r[k]) for r in rep) for k in ("trials", "steps", "failures")}
    m = {
        k: max((int(r[k]) for r in rep), default=0)
        for k in ("blocks", "edges", "paths", "assumes", "frontier")
    }
    return (
        f"fuzz-par.progress: {t:.3f} reported={len(rep)}/{len(procs)}"
        f" trials-sum={s['trials']} steps-sum={s['steps']}"
        f" failures-sum={s['failures']} blocks-max={m['blocks']}"
        f" edges-max={m['edges']} paths-max={m['paths']}"
        f" assumes-max={m['assumes']} frontier-max={m['frontier']}"
    )


def read_cov(path):
    """A --fuzz-coverage-out file: (totals fields, {kind: set of ids}, model).

    model: frontier (deepest), gates {id: fields}, cmps {id: (pred, counts)}.
    """
    totals, ids = {}, {"block": set(), "edge": set(), "assume": set(), "path": set()}
    model = {"frontier": 0, "gate": {}, "cmp": {}}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, rest = line.partition(": ")
            if key == "totals":
                totals = fields(rest)
            elif key in ids:
                ids[key].add(rest)
            elif key == "frontier":
                model["frontier"] = int(fields(rest)["deepest"])
            elif key == "gate":
                model["gate"][rest.split()[0]] = fields(rest)
            elif key == "cmp":
                f_ = fields(rest.split(" at=")[0])
                counts = [int(c) for c in f_["counts"].split(",")]
                model["cmp"][rest.split()[0]] = counts
            else:
                raise ValueError(f"{path}: unexpected line '{line}'")
    return totals, ids, model


def join_model(acc, m):
    """Joins one file's model coverage into `acc` (the same shape)."""
    acc["frontier"] = max(acc["frontier"], m["frontier"])
    for g, f in m["gate"].items():
        a = acc["gate"].setdefault(
            g, {"op": f.get("op", "-"), "passed": 0, "blocked": 0, "distance": "-"}
        )
        a["passed"] += int(f["passed"])
        a["blocked"] += int(f["blocked"])
        d = f.get("distance", "-")
        if d != "-" and (a["distance"] == "-" or int(d) < int(a["distance"])):
            a["distance"] = d
    for c, counts in m["cmp"].items():
        a = acc["cmp"].setdefault(c, [0] * len(counts))
        for k, n in enumerate(counts):
            a[k] += n


def emit(line):
    print(line, flush=True)


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    if "--agent" in sys.argv[: sys.argv.index("--") if "--" in sys.argv else None]:
        sys.stdout.write(AGENT)
        return 0
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", type=int, required=True, help="processes")
    ap.add_argument("--out", required=True, help="new or empty directory")
    ap.add_argument("--seed-base", type=int, default=1)
    ap.add_argument("--sea-run", default="sea-run")
    ap.add_argument("--progress", type=float, default=0)
    ap.add_argument("--wait-all", action="store_true")
    ap.add_argument("--sync", action="store_true", help="corpus sync")
    ap.add_argument("--sync-period", type=float, help="seconds (with --sync)")
    ap.add_argument("args", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    args = a.args[1:] if a.args[:1] == ["--"] else a.args
    if a.n < 1:
        ap.error("-n must be at least 1")
    if a.progress < 0:
        ap.error("--progress must not be negative")
    if not args:
        ap.error("give the program and the sea-run args after --")
    if a.sync_period is not None and not a.sync:
        ap.error("--sync-period needs --sync")
    if a.sync_period is not None and a.sync_period <= 0:
        ap.error("--sync-period must be positive")
    reserved = RESERVED + (SYNC_RESERVED if a.sync else ())
    for x in args:
        if "--" + x.split("=")[0].lstrip("-") in reserved:
            ap.error(f"{x}: the launcher sets {', '.join(reserved)}")
    if os.path.exists(a.out) and (not os.path.isdir(a.out) or os.listdir(a.out)):
        ap.error(f"--out {a.out} is not a new or empty directory")

    start = time.monotonic()
    events = queue.Queue()
    procs = []
    try:
        for i in range(a.n):
            pdir = os.path.join(a.out, f"p{i}")
            os.makedirs(pdir)
            seed = a.seed_base + i
            cmd = [a.sea_run, *args, f"--seed={seed}"]
            cmd += [f"--fuzz-out={os.path.join(pdir, 'out')}"]
            cmd += [f"--fuzz-coverage-out={os.path.join(pdir, 'cov.txt')}"]
            if a.sync and a.n > 1:
                peers = [
                    os.path.join(a.out, f"p{j}", "out") for j in range(a.n) if j != i
                ]
                cmd.append(f"--fuzz-sync={','.join(peers)}")
                if a.sync_period is not None:
                    cmd.append(f"--fuzz-sync-period={a.sync_period:g}")
            if a.progress > 0:
                cmd.append(f"--fuzz-progress={a.progress:g}")
            p = Proc(i, seed, cmd, pdir)
            threading.Thread(target=reader, args=(p, events), daemon=True).start()
            procs.append(p)

        first = None
        open_streams = len(procs)
        next_tick = a.progress
        while open_streams:
            timeout = None
            if a.progress > 0:
                timeout = max(0.0, start + next_tick - time.monotonic())
            try:
                p, line, wall = events.get(timeout=timeout)
            except queue.Empty:
                p = None
            if p is not None and line is None:
                open_streams -= 1
            elif p is not None:
                key, _, rest = line.partition(": ")
                if key == "fuzz.progress":
                    p.progress = fields(rest)
                elif key == "fuzz.trials":
                    p.trials = fields(rest)
                    p.trials["trials"] = rest.split()[0]
                elif key == "fuzz.coverage":
                    p.coverage = fields(rest)
                elif key == "fuzz.corpus":
                    p.corpus = fields(rest)
                    p.corpus["entries"] = rest.split()[0]
                elif key == "fuzz.sync":
                    p.sync = fields(rest)
                elif key in ("fuzz.failure", "fuzz.memory-safety"):
                    p.failed = True
                    if first is None:
                        f = fields(rest)
                        first = (p, key.split(".")[1], f, wall - start)
                        if not a.wait_all:
                            for q in procs:
                                if q is not p and q.popen.poll() is None:
                                    q.stopped = True
                                    q.popen.terminate()
            if a.progress > 0 and time.monotonic() >= start + next_tick:
                emit(merged(procs, time.monotonic() - start))
                while next_tick <= time.monotonic() - start:
                    next_tick += a.progress
    finally:
        for p in procs:
            if p.popen.poll() is None:
                p.popen.terminate()
        for p in procs:
            if p.popen.poll() is None:
                p.popen.wait()

    rc = 0
    for p in procs:
        code = p.popen.wait()
        p.err.close()
        if code == 4:
            rc = max(rc, 4)
        elif code != 0 and not p.stopped:
            p.errored = True
            with open(os.path.join(p.dir, "stderr.txt")) as f:
                msg = f.readline().strip()
            emit(f"fuzz-par.error: process={p.i} exit={code} stderr={msg}")
            rc = 1
        p.code = code

    for p in procs:
        if p.trials and p.coverage:
            src, d = (
                "final",
                {
                    **p.coverage,
                    "trials": p.trials["trials"],
                    "steps": p.trials.get("steps", "-"),
                },
            )
        elif p.progress:
            src, d = "progress", p.progress
        else:
            src, d = "none", {}
        end = (
            "failure"
            if p.failed
            else "stopped"
            if p.stopped
            else "error"
            if p.errored
            else "done"
        )
        emit(
            f"fuzz-par.process: {p.i} seed={p.seed} exit={p.code} end={end}"
            f" from={src}"
            + "".join(
                f" {k}={d.get(k, '-')}"
                for k in ("trials", "steps", "blocks", "edges", "paths", "assumes")
            )
            + f" corpus={(p.corpus or {}).get('entries', d.get('corpus', '-'))}"
            + (
                "".join(
                    f" {name}={(p.sync or {}).get(k, '-')}"
                    for name, k in (
                        ("imported", "imported"),
                        ("sync-kept", "kept"),
                        ("sync-skipped", "skipped"),
                    )
                )
                if a.sync
                else ""
            )
        )

    if first:
        p, kind, f, wall = first
        line = (
            f"fuzz-par.first-failure: process={p.i} seed={p.seed} kind={kind}"
            f" trial={f.get('trial', '-')} time={f.get('time', '-')}"
            f" wall={wall:.3f} at={f.get('at', '-')}"
        )
        if "file" in f:
            line += f" file={f['file']}"
        emit(line)
    else:
        emit("fuzz-par.first-failure: none")

    union = {"block": set(), "edge": set(), "assume": set(), "path": set()}
    model = {"frontier": 0, "gate": {}, "cmp": {}}
    totals, capped, joined = {}, False, 0
    for p in procs:
        if p.stopped or not os.path.exists(p.cov):
            continue
        t, ids, m = read_cov(p.cov)
        if totals and (t.get("blocks-total"), t.get("assumes-total")) != (
            totals.get("blocks-total"),
            totals.get("assumes-total"),
        ):
            emit(f"fuzz-par.error: process={p.i} coverage of another program")
            return 1
        totals = t
        capped |= t.get("paths-capped") == "1"
        for k in union:
            union[k] |= ids[k]
        join_model(model, m)
        joined += 1
    emit(
        f"fuzz-par.union: blocks={len(union['block'])}"
        f" blocks-total={totals.get('blocks-total', '-')}"
        f" edges={len(union['edge'])} assumes={len(union['assume'])}"
        f" assumes-total={totals.get('assumes-total', '-')}"
        f" paths={len(union['path'])}"
        + (" paths-capped=1" if capped else "")
        + f" processes={joined}/{len(procs)}"
    )
    if model["gate"]:
        closed = [
            g
            for g, f in sorted(model["gate"].items(), key=lambda x: int(x[0]))
            if f["op"] == "sea.assume" and not f["passed"] and f["blocked"]
        ]
        line = (
            f"fuzz-par.model: frontier-max={model['frontier']}"
            f" closed={len(closed)} closed-gates={','.join(closed) or '-'}"
        )
        if model["cmp"]:
            hit = sum(1 for cs in model["cmp"].values() for n in cs if n)
            total = sum(len(cs) for cs in model["cmp"].values())
            line += f" cmp-buckets={hit}/{total}"
            dists = [
                f"{g}:{f['distance']}"
                for g, f in sorted(model["gate"].items(), key=lambda x: int(x[0]))
                if f["distance"] != "-" and not f["passed"]
            ]
            line += f" closed-distance={','.join(dists) or '-'}"
        emit(line)
    emit(
        f"fuzz-par.summary: processes={len(procs)}"
        f" wall={time.monotonic() - start:.3f}"
        f" failures={sum(p.failed for p in procs)}"
        f" sync={'on' if a.sync else 'off'}"
    )
    return rc


if __name__ == "__main__":
    sys.exit(main())
