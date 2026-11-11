#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Run several `sea-run --fuzz` processes on one program (fuzz-par.py --agent).

The fuzzer is single-threaded; this runs N processes with distinct seeds,
each with its own --fuzz-out directory and --fuzz-coverage-out file, merges
their fuzz.progress lines, reports the first failure and the coverage of
each process and of their union. No exchange between the processes.
"""

import argparse
import os
import queue
import re
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
      [--progress S] [--wait-all] -- <program.mlir> <sea-run args>...
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
OUTPUT (key: value lines, stdout):
  fuzz-par.progress: <wall s> reported=<k>/<N> trials-sum=<n> steps-sum=<n>
      failures-sum=<n> blocks-max=<n> edges-max=<n> paths-max=<n>
      assumes-max=<n>
      (the latest fuzz.progress of each process that has printed one; sums
       of the cost, max of the coverage: the union needs the ids, at the end)
  fuzz-par.process: <i> seed=<s> exit=<rc> end=done|failure|stopped
      from=final|progress|none trials=<n> steps=<n> blocks=<n> edges=<n>
      paths=<n> assumes=<n>
      (from: the process's fuzz.trials / fuzz.coverage lines, or its last
       fuzz.progress if it was stopped, or nothing; stopped: by the launcher)
  fuzz-par.first-failure: process=<i> seed=<s> kind=failure|memory-safety
      trial=<n> time=<s> wall=<s> at=<where> [file=<path>]
      (time: the process's own clock from its first trial; wall: the
       launcher's, from the start, when the line arrived; or `none`)
  fuzz-par.union: blocks=<n> blocks-total=<n> edges=<n> assumes=<n>
      assumes-total=<n> paths=<n> [paths-capped=1] processes=<k>/<N>
      (union of the coverage files of the processes that ended on their
       own; stopped processes write none)
  fuzz-par.summary: processes=<N> wall=<s> failures=<processes with one>
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
        self.progress = None  # fields of the latest fuzz.progress
        self.trials = None  # fields of fuzz.trials
        self.coverage = None  # fields of fuzz.coverage
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
        for k in ("blocks", "edges", "paths", "assumes")
    }
    return (
        f"fuzz-par.progress: {t:.3f} reported={len(rep)}/{len(procs)}"
        f" trials-sum={s['trials']} steps-sum={s['steps']}"
        f" failures-sum={s['failures']} blocks-max={m['blocks']}"
        f" edges-max={m['edges']} paths-max={m['paths']}"
        f" assumes-max={m['assumes']}"
    )


def read_cov(path):
    """A --fuzz-coverage-out file: (totals fields, {kind: set of ids})."""
    totals, ids = {}, {"block": set(), "edge": set(), "assume": set(), "path": set()}
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
            else:
                raise ValueError(f"{path}: unexpected line '{line}'")
    return totals, ids


def emit(line):
    print(line, flush=True)


def main():
    if "--agent" in sys.argv:
        sys.stdout.write(AGENT)
        return 0
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", type=int, required=True, help="processes")
    ap.add_argument("--out", required=True, help="new or empty directory")
    ap.add_argument("--seed-base", type=int, default=1)
    ap.add_argument("--sea-run", default="sea-run")
    ap.add_argument("--progress", type=float, default=0)
    ap.add_argument("--wait-all", action="store_true")
    ap.add_argument("args", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    args = a.args[1:] if a.args[:1] == ["--"] else a.args
    if a.n < 1:
        ap.error("-n must be at least 1")
    if a.progress < 0:
        ap.error("--progress must not be negative")
    if not args:
        ap.error("give the program and the sea-run args after --")
    for x in args:
        if x.split("=")[0] in RESERVED:
            ap.error(f"{x}: the launcher sets {', '.join(RESERVED)}")
    if os.path.exists(a.out) and (not os.path.isdir(a.out) or os.listdir(a.out)):
        ap.error(f"--out {a.out} is not a new or empty directory")

    start = time.monotonic()
    events = queue.Queue()
    procs = []
    for i in range(a.n):
        pdir = os.path.join(a.out, f"p{i}")
        os.makedirs(pdir)
        seed = a.seed_base + i
        cmd = [a.sea_run, *args, f"--seed={seed}"]
        cmd += [f"--fuzz-out={os.path.join(pdir, 'out')}"]
        cmd += [f"--fuzz-coverage-out={os.path.join(pdir, 'cov.txt')}"]
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

    rc = 0
    for p in procs:
        code = p.popen.wait()
        p.err.close()
        if code == 4:
            rc = max(rc, 4)
        elif code != 0 and not p.stopped:
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
        end = "failure" if p.failed else "stopped" if p.stopped else "done"
        emit(
            f"fuzz-par.process: {p.i} seed={p.seed} exit={p.code} end={end}"
            f" from={src}"
            + "".join(
                f" {k}={d.get(k, '-')}"
                for k in ("trials", "steps", "blocks", "edges", "paths", "assumes")
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
    totals, capped, joined = {}, False, 0
    for p in procs:
        if p.stopped or not os.path.exists(p.cov):
            continue
        t, ids = read_cov(p.cov)
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
    emit(
        f"fuzz-par.summary: processes={len(procs)}"
        f" wall={time.monotonic() - start:.3f}"
        f" failures={sum(p.failed for p in procs)}"
    )
    return rc


if __name__ == "__main__":
    sys.exit(main())
