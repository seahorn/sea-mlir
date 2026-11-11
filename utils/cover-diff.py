#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""cover-diff: validate a change of the lowering by reach coverage.

Two configurations A and B lower the same input program (sea-vc argument
sets, e.g. B = the default and A = --lower-opt=false, or A =
--lower-disable=<pass>); the change from A to B is validated by what the
two lowered programs let reach their check sites.

Targets. sea-vc --reach tags every sea.assume and sea.assert of the input
before any pass (sea.reach_site, its position in program order) and
reports it on each target as site=<k>. A target of A matches the target
of B with the same key, the site and the role:
  assume   the input assume k, or the sea.unreachable its folding to false
           left (the same top of op);
  assert   the input assert k, kept;
  assume   for an input assert k also: the assume sea-merge-asserts makes
           of it (reached once the assert holds). An assert merged on one
           side only is two keys, (k, assert) and (k, assume), each on one
           side: they never match each other;
  fail     the failure of the input assert k: an execution reaches it with
           its condition false (sea-vc's reach-site marker of=sea.assert.fail
           in a block of its own: sea-split-assert-failures, or the landing
           block of sea-merge-asserts).
So both the reach of every check and whether each assert can fail are
compared; the program's safety verdict is too (sea-vc without --reach on
each side, the `verdict:` line; sat against unsat is a DIFF; --no-verdict
leaves it out).
Both sides run with sea-vc --reach-keep-sites (--no-keep-sites: not): the
canonicalizer replaces a tagged assume / assert of the constant true by a
reach-site marker (sea.print, inert in the VC) with its site instead of
erasing it, so the check stays a target, with the role of the check it
replaced (of=). Each side's lowering is then the normal one plus these
markers. Without them, such a check is on one side only (removed-true).
Several targets with one key on one side (a pass that copies code) are
one target: reachable if one is, unreachable if all are, else unknown.
Targets without a site (checks the lowering made) are counted (untagged=)
and not compared. A job with no tagged target on either side (an input
without checks, a sea-vc that reports no site=) is unknown
(reason=no-targets), not a pass. --reach options in --args / --a / --b are
refused (cover-diff sets them; block targets have no sites). The reach answer of a target whose replay stopped at a
memory-safety error (replay=memory-error: it rests on A-INB, which the
run breaks) counts as unknown.

Placement. Both sides run on one placement of the objects (sea-vc
--place-out / --place-in): the writer side lowers with --place-out=<file>,
the other with --place-in=<file>, so their common objects are at the same
addresses (and their obj.<name> symbols have the same values). The writer
is the side with more objects: A first; if B has an object A's file does
not name (or one of another kind, size or align: place.in-unplaced / place.in-mismatch), the reverse, once.
If both fail, there is no shared placement: the job is unknown
(reason=layout), never a pass. The gate does not check all placements:
both lowerings are compared on one placement, the writer's; the reader
takes it as an allocator would (checked for alignment, size, kind,
overlap, null and width). The reach
runs, the safety verdicts and the R_T programs are each lowered so, the
writer of the reach runs first; a witness replays on the other side on
the reach runs' file.

Level 0, coverage equality (always): sea-vc --reach on A and on B, each
with its own concrete runs first (--fuzz trials, --corpus seeds; the
inputs of the two sides need not be the same), the solver for the rest.
Per key:
  equal                  both reachable, or both unreachable;
  DIFF                   reachable on one side, unreachable on the other;
  removed-ok             on one side only, unreachable there (dead code the
                         other side's lowering erased);
  removed-true           on one side only, reachable there, and an assume
                         or assert of the constant true there (sea-vc
                         cond=true; canonicalize erases it): such a check
                         is reached iff the next target of its block is
                         (next=<site>), so it passes when that next target
                         is equal (or removed-true itself); with no next
                         target, or one that is not equal, it is unknown
                         (--unresolved-true=pass: removed-true; the
                         `target:` line says resolved=next|unresolved);
  removed-but-reachable  on one side only, reachable there (a DIFF);
  unknown                unknown on a side that decides the class.
A DIFF (or removed-but-reachable) with a reachable side is shown
concretely: that side's witness (inputs by nondet site and memory cell,
sea-vc --reach-witness) runs on the other side's lowering (sea-vc --reach
--reach-query=false --reach-corpus=<witness>), and the `replay:` line says
whether it reaches the key there and which keys it does reach.

Level 1, reachability equivalence (--level=1): for every key reachable on
both sides (a key with copies on a side is unknown, reason=copies: R_T of
one copy is not the target), R_T of A and R_T of B (sea-vc
--reach-programs; R_T = the lowered program with the other asserts
dropped and assert(false) at T, its assert tagged sea.reach_target) are
compared by query-diff's classifier with --project-blocks (sea-vc R_T
--smt2 <--rt-args>; default: --args without the lowering's flags): equiv-sat says that the same inputs reach T on both
sides (block guards projected: a forall-exists check). An object one
lowering erased leaves its address, obj.<name> = <base>, in the other's
query only: query-diff substitutes such a symbol (a symbol obj.<name> declared in one
query only, defined as a numeral; subst=<n>), it is no input. The R_T are paired
by key, from the reach.program records. identical / order / equiv-sat
pass; DIFF fails; unknown is unknown; both-unsat and error are errors
(level 0 answered both reachable).
Skolem hints: sea-vc --reach gives every input block a stable id before
any pass (sea.block_ids on its terminator; the passes keep them: a merged
block holds both lists, a split tail the id <last>.s<k>, a new block a derived
id) and R_T keeps them, so each R_T query names the ids of its block
guards (`; sea.block-ids`). When both queries of a pair carry them,
query-diff first runs the two quantifier-free checks with the guards of
one side replaced by terms over the other's (query-diff derive_hints());
a hint that fails (sat or unknown) falls back to the quantified check, and
says so (hints=<direction>:<answer>). A hint is checked, not trusted: it
never makes a pass. Each level1 line says how its pair was proved: by=identical
(the same query up to order), by=names (equivalent with the guards matched
by name), by=hints, by=exists. --no-hints: the quantified check only.

Exit: 0 pass; 1 some DIFF (level 0 or 1, or the verdicts); 3 an error
(sea-vc or z3 failed); 2 some unknown; checked in this order, for a job's
verdict and for the run.
"""

import argparse
import concurrent.futures as cf
import glob
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time

AGENT = """\
cover-diff: validate a lowering change by reach coverage (level 0) and
  reachability equivalence per target (level 1).
WHY: a pass must keep which check sites can be reached, except the ones it
  proved dead; two coverage runs, cheap; level 1 adds query-diff on R_T.
USAGE:
  utils/cover-diff.py --sea-vc <sea-vc> --a="<A args>" --b="<B args>"
      [--args="--lower"] [--fuzz 1000] [--seed 0] [--corpus <dir|file>]
      [--level 0|1] [--rt-args="<R_T args>"] [--z3 <z3>] [--no-hints]
      [--rlimit 200000000] [--reach-rlimit <n>] [--unresolved-true unknown|pass]
      [--no-verdict]
      [--no-keep-sites] [--par 4] [--timeout 600] [--keep <dir>]
      [--b-input <file>] <f.mlir | dir>...
  e.g. --a=--lower-opt=false --b=   (validates the canonicalize + cse folds)
       --a=--lower-disable=canonicalize --b=
  --no-keep-sites: without sea-vc --reach-keep-sites (checks of true are
  erased, not kept as reach-site markers: removed-true / unknown).
  --b-input: B lowers another file (one input; both files must carry
  sea.reach_site tags, which match the checks: never tagged by position).
  --rt-args: default --args without the lowering's flags (--lower*,
  --widen-memory*, --check-*, --print-refs).
KEYS: site/role, site = sea.reach_site of the input check (sea-vc --reach
  site=), role assume|assert|fail (a folded assume's sea.unreachable is its
  assume; sea-merge-asserts' assume of assert k is k/assume; k/fail: assert
  k reached with its condition false, sea-vc of=sea.assert.fail).
  Also compared: the safety verdicts (verdict: line; --no-verdict: not).
  No tagged target on either side: the job is unknown (reason=no-targets).
CLASSES (level 0, per key):
  equal                  same answer on both sides (reachable / unreachable)
  DIFF                   reachable on one side, unreachable on the other
  removed-ok             on one side only, unreachable there
  removed-true           on one side only, a check of the constant true
                         (cond=true), and the next target of its block
                         (next=, reached iff it is) is equal; else unknown
                         (--unresolved-true=pass: removed-true too)
  removed-but-reachable  on one side only, reachable there (a DIFF)
  unknown                an unknown answer decides the class (also a
                         reachable answer with replay=memory-error)
PLACEMENT: both sides on one placement (sea-vc --place-out on the writer,
  --place-in on the other): A writes; if B has an object A's file lacks
  (place.in-unplaced or place.in-mismatch), B writes and A reads, once; both fail: the job is
  unknown reason=layout (level 1 / verdict alone: those unknown). Applies to
  the reach runs (level 0), the verdicts, the R_T programs and the replays.
LEVEL 1 (per key reachable on both sides): query-diff --project-blocks on
  R_T(A) vs R_T(B): identical / order / equiv-sat (the same inputs reach
  T) pass, DIFF fails, unknown, both-unsat / error are errors; a key with
  copies on a side is unknown (reason=copies). A symbol obj.<name>
  declared in one query only and defined as a numeral is substituted
  (subst=<n>); other one-sided symbols are inputs. Block guards: by name, then
  the Skolem hints from the block ids of the two R_T (quantifier-free,
  checked), then the quantified check (--no-hints: no hints).
OUTPUT (stdout, line-oriented):
  config: a=<args> b=<args> args=<args> fuzz=<n> seed=<n> level=<n>
      rt-args=<args> unresolved-true=unknown|pass keep-sites=0|1
  layout: <job> writer=a|b [reversed <side>-in=<place.in-kind>]
  layout: <job> [verdict] unknown reason=layout a-in=<kind> b-in=<kind>
  target: <job> <site>/<role> <class> a=<status> b=<status>
      [cond=true resolved=next|unresolved] [a-copies=<n>] [b-copies=<n>]
      [witness=<side>:<file>]
  replay: <job> <site>/<role> on=<a|b> witness=<file> reached=yes|no
      [keys=<site>/<role>,...]   (the witness's run on the other side)
  verdict: <job> equal|DIFF|other a=<result> b=<result>  (other: a side
      unknown or inconclusive: the job is unknown; a sea-vc exit other
      than 0 / 3 / 4 is an error)
  level1: <job> <site>/<role> <class> [reason=...] [old=.. new=..]
      [subst=<n>] [by=identical|names|hints|exists] [hints=<old-new|new-old>:<answer>
      | hints=incomplete:<n> | hints=none:<why>]   (hints=: why the quantified check ran)
  job: <job> <pass|DIFF|unknown|error> targets=<n> equal=<n> diff=<n>
      removed-ok=<n> removed-true=<n> removed-but-reachable=<n> unknown=<n>
      untagged=<a>/<b> [level1: equiv=<n> diff=<n> unknown=<n> error=<n>] [reason=<r>]
      time=<s>
  total: jobs=<n> pass=<n> diff=<n> unknown=<n> error=<n>
  targets: equal=<n> diff=<n> removed-ok=<n> removed-true=<n>
      removed-but-reachable=<n> unknown=<n> untagged=<a>/<b>
  [level1: equiv=<n> diff=<n> unknown=<n> error=<n> pairs=<n>
      by-identical=<n> by-names=<n> by-hints=<n> by-exists=<n>]
  status: pass|DIFF|unknown|error
STATUS: 0 pass; 1 some DIFF; 3 some error; 2 some unknown (checked in
  this order, per job and for the run).
--keep <dir>: keep each job's witnesses and R_T programs in
  <dir>/<input path, / as _> (else removed). Jobs are named by basename,
  by path when two inputs share one.
"""

QD = None  # utils/query-diff.py, loaded on first use (level 1)
L1_CLASSES = ("equiv", "diff", "unknown", "error")
# and how each equivalence was proved
L1_KEYS = L1_CLASSES + ("by-identical", "by-names", "by-hints", "by-exists")


QD_LOCK = threading.Lock()


def query_diff():
    """utils/query-diff.py, loaded once (jobs run in threads: published only
    once loaded)."""
    global QD
    with QD_LOCK:
        if QD is None:
            path = os.path.join(
                os.path.dirname(os.path.abspath(__file__)), "query-diff.py"
            )
            spec = importlib.util.spec_from_file_location("query_diff", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            QD = mod
    return QD


class SeaVcError(Exception):
    pass


class LayoutError(SeaVcError):
    """A side could not take the other side's placement (--place-in:
    place.in-unplaced or place.in-mismatch, such as an object the file does
    not name)."""


class NoLayout(Exception):
    """Neither side's placement fits the other: no shared layout."""


PLACE_IN_ERROR = re.compile(r"\[(place\.in-(?:unplaced|mismatch))\]")


def check_layout(p):
    """Raises LayoutError if the run failed on --place-in."""
    m = PLACE_IN_ERROR.search(p.stderr)
    if p.returncode == 1 and m:
        raise LayoutError(m.group(1))


def role(rec):
    """The role of a reach record: a folded assume's sea.unreachable is
    its assume (the same top of op), a reach-site marker (sea.print,
    --reach-keep-sites) the check of true it replaced (of=)."""
    op = rec.get("op", "")
    if op == "sea.print":
        op = rec.get("of", "")
    if op in ("sea.assume", "sea.unreachable"):
        return "assume"
    if op == "sea.assert":
        return "assert"
    if op == "sea.assert.fail":
        return "fail"
    raise SeaVcError(f"reach target of op {op!r}")


def first_error(stderr):
    """The first error line of a run's stderr, else its last line (a
    warning summary follows the error), as one token."""
    err = stderr.strip().splitlines()
    line = next((x for x in err if "error" in x.lower()), err[-1] if err else "")
    return line[:80].replace(" ", "_")


def run_sea_vc(a, argv, timeout, ok=(0, 3, 4)):
    """The JSON records of a sea-vc run (--format=jsonl)."""
    cmd = [a.sea_vc] + argv + ["--format=jsonl"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SeaVcError("timeout") from None
    # --reach: 0 decided; 3 some target unknown; 4 a replay at a memory error
    if p.returncode not in ok:
        check_layout(p)
        last = first_error(p.stderr)
        raise SeaVcError(f"sea-vc-exit-{p.returncode}" + (":" + last if last else ""))
    recs = []
    for line in p.stdout.splitlines():
        try:
            recs.append(json.loads(line))
        except json.JSONDecodeError:
            raise SeaVcError("bad-jsonl") from None
    return recs


def status_of(recs):
    """Combine the records of one key: reachable / unreachable / unknown."""
    st = []
    for r in recs:
        s = r.get("status")
        if s == "reachable" and r.get("replay") == "memory-error":
            s = "unknown"
        st.append(s)
    if "reachable" in st:
        return "reachable"
    if all(s == "unreachable" for s in st):
        return "unreachable"
    return "unknown"


def collect(recs):
    """key -> [records], and the number of untagged targets."""
    by, untagged = {}, 0
    for r in recs:
        if r.get("kind") != "reach":
            continue
        if "site" not in r:
            untagged += 1
            continue
        by.setdefault((r["site"], role(r)), []).append(r)
    return by, untagged


def key_text(k):
    return f"{k[0]}/{k[1]}"


def classify(sa, sb):
    """The level-0 class of a key from the status of each side (None: the
    key is not on that side)."""
    if sa is None or sb is None:
        s = sa if sb is None else sb
        if s == "unreachable":
            return "removed-ok"
        if s == "reachable":
            return "removed-but-reachable"
        return "unknown"
    if "unknown" in (sa, sb):
        return "unknown"
    return "equal" if sa == sb else "DIFF"


def resolve_true(ma, mb, classes, unresolved_pass):
    """removed-but-reachable keys whose one target is a check of the
    constant true (cond=true; canonicalize erases it): such a target is
    reached iff the next target of its block is (next=<site>), so it is
    removed-true when that next target is equal on both sides (or is
    removed-true itself), and unknown otherwise (no next target, or one
    that is not equal; removed-true with --unresolved-true=pass). Returns
    {key: "next" | "unresolved"} for the keys made removed-true or unknown
    so."""
    how = {}
    memo = {}

    def ok(k, seen):
        if k in memo:
            return memo[k]
        if classes.get(k) == "equal":
            return True
        if classes.get(k) != "removed-but-reachable" or k in seen:
            return False
        recs = ma.get(k) or mb.get(k)
        side = ma if k in ma else mb
        r = recs[0]
        res = False
        if len(recs) == 1 and r.get("cond") == "true" and "next" in r:
            nk = [x for x in side if x[0] == r["next"]]
            res = len(nk) == 1 and ok(nk[0], seen | {k})
        memo[k] = res
        return res

    for k, c in list(classes.items()):
        if c != "removed-but-reachable":
            continue
        recs = ma.get(k) or mb.get(k)
        if len(recs) == 1 and recs[0].get("cond") == "true":
            if ok(k, frozenset()):
                classes[k], how[k] = "removed-true", "next"
            else:
                how[k] = "unresolved"
                classes[k] = "removed-true" if unresolved_pass else "unknown"
    return how


def side_argv(a, f, side_args, extra):
    keep = ["--reach-keep-sites"] if a.keep_sites else []
    return [f] + a.common + side_args + ["--reach", "--reach-failures"] + keep + extra


def placed(a, work, tag, run, first="a"):
    """The runs of the two sides on one placement: the writer side
    (--place-out=<file>) first, then the other (--place-in=<file>). run(side,
    place_args) runs a side. The writer should be the side with more
    objects: `first`; if the other cannot take its placement (LayoutError:
    an object the file does not name, a size that differs), the reverse,
    once. Returns ({side: result}, writer, file, note), note the first
    try's failure (or ""); raises NoLayout if both fail."""
    failed = {}
    for writer in (first, "b" if first == "a" else "a"):
        reader = "b" if writer == "a" else "a"
        f = os.path.join(work, f"{tag}-{writer}.place")
        out = {writer: run(writer, [f"--place-out={f}"])}
        try:
            out[reader] = run(reader, [f"--place-in={f}"])
        except LayoutError as e:
            failed[reader] = str(e)
            continue
        note = "".join(f"reversed {k}-in={v}" for k, v in failed.items())
        return out, writer, f, note
    raise NoLayout(" ".join(f"{k}-in={v}" for k, v in sorted(failed.items())))


def reach_side(a, f, side_args, wdir, place=()):
    extra = [f"--reach-witness={wdir}"] + list(place)
    if a.reach_rlimit:
        extra += [f"--reach-rlimit={a.reach_rlimit}"]
    if a.fuzz:
        extra += [f"--reach-fuzz={a.fuzz}", f"--reach-seed={a.seed}"]
    if a.corpus:
        extra += ["--reach-corpus=" + ",".join(a.corpus)]
    return run_sea_vc(a, side_argv(a, f, side_args, extra), a.timeout)


def replay(a, f, side_args, witness, place):
    """The keys the witness's run reaches on a side (no query), on the
    placement of the witness's run (place: --place-in=<file>)."""
    recs = run_sea_vc(
        a,
        side_argv(
            a,
            f,
            side_args,
            ["--reach-query=false", f"--reach-corpus={witness}", "--reach-fuzz=0"]
            + place,
        ),
        a.timeout,
    )
    by, _ = collect(recs)
    return sorted(k for k, rs in by.items() if any(r.get("via") == "run" for r in rs))


def rt_programs(a, f, side_args, d, place=()):
    """key -> R_T file, for the keys with one target on a side."""
    recs = run_sea_vc(
        a,
        side_argv(a, f, side_args, [f"--reach-programs={d}"] + list(place)),
        a.timeout,
    )
    out = {}
    for r in recs:
        if r.get("kind") == "reach.program" and "site" in r:
            out.setdefault((r["site"], role(r)), []).append(r["file"])
    return out


def smt2(a, rt):
    """(query, None) or (None, why) for R_T."""
    cmd = [a.sea_vc, rt] + a.rt_args + ["--smt2"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    if p.returncode == 0 and p.stdout:
        return p.stdout, None
    return None, f"sea-vc-exit-{p.returncode}:" + first_error(p.stderr)


def safety_verdict(a, f, side_args, place=()):
    """The sea-vc safety verdict of a side: sat / unsat / unknown, or
    inconclusive (exit 4: a sat that rests on A-INB, which its run breaks).
    Any other exit (1 error, 2 a VC / interpreter mismatch, 5 an invalid
    rewrite) is an error."""
    cmd = [a.sea_vc, f] + a.common + side_args + list(place) + ["--format=jsonl"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout)
    except subprocess.TimeoutExpired:
        raise SeaVcError("verdict-timeout") from None
    if p.returncode not in (0, 3, 4):
        check_layout(p)
        raise SeaVcError(f"verdict-sea-vc-exit-{p.returncode}:" + first_error(p.stderr))
    for line in p.stdout.splitlines():
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            raise SeaVcError("bad-jsonl") from None
        if r.get("kind") == "result":
            return "inconclusive" if p.returncode == 4 else r.get("value", "none")
    raise SeaVcError("no-result")


# sea-vc flags of the lowering: not for the R_T queries (R_T is lowered)
LOWER_FLAGS = (
    "--lower",
    "--lower-opt",
    "--lower-disable",
    "--widen-memory",
    "--widen-memory-report",
    "--check-align",
    "--check-ptr-index",
    "--print-refs",
)


def rt_args_of(common):
    return [x for x in common if x.split("=", 1)[0] not in LOWER_FLAGS]


def one(f, a):
    """The lines of one job, and its counts."""
    job = a.names[f]
    fb = a.b_input or f
    t0 = time.time()
    lines = []
    counts = {
        k: 0
        for k in (
            "equal",
            "DIFF",
            "removed-ok",
            "removed-true",
            "removed-but-reachable",
            "unknown",
        )
    }
    l1 = dict.fromkeys(L1_KEYS, 0)
    untagged = [0, 0]
    verdict, reason = "pass", ""
    work = None
    verdict_diff = verdict_unknown = no_targets = False
    try:
        if a.keep:
            # one directory per input, by its full path
            work = os.path.join(a.keep, os.path.abspath(f).strip("/").replace("/", "_"))
            shutil.rmtree(work, ignore_errors=True)
            os.makedirs(work)
        else:
            work = tempfile.mkdtemp(prefix=f"cover-diff-{os.path.basename(job)}-")
        inputs = {"a": (f, a.a), "b": (fb, a.b)}
        # one placement for both sides (the writer's): the same objects at
        # the same addresses, so that addresses are no inputs that differ
        # (level 1) and a witness means the same on the other side
        try:
            r, writer, layout, note = placed(
                a,
                work,
                "reach",
                lambda side, pl: reach_side(
                    a, *inputs[side], os.path.join(work, f"{side}-w"), pl
                ),
            )
        except NoLayout as e:
            lines.append(f"layout: {job} unknown reason=layout {e}")
            raise
        lines.append(f"layout: {job} writer={writer}" + (f" {note}" if note else ""))
        ra, rb = r["a"], r["b"]
        ma, untagged[0] = collect(ra)
        mb, untagged[1] = collect(rb)
        both_reachable, copies = [], []
        keys = sorted(set(ma) | set(mb))
        classes = {}
        for k in keys:
            sa = status_of(ma[k]) if k in ma else None
            sb = status_of(mb[k]) if k in mb else None
            classes[k] = classify(sa, sb)
        how = resolve_true(ma, mb, classes, a.unresolved_true == "pass")
        for k in keys:
            sa = status_of(ma[k]) if k in ma else None
            sb = status_of(mb[k]) if k in mb else None
            cls = classes[k]
            counts[cls] += 1
            parts = [
                "target:",
                job,
                key_text(k),
                cls,
                f"a={sa or 'absent'}",
                f"b={sb or 'absent'}",
            ]
            if k in how:
                parts.append(f"cond=true resolved={how[k]}")
            for side, m in (("a", ma), ("b", mb)):
                if k in m and len(m[k]) > 1:
                    parts.append(f"{side}-copies={len(m[k])}")
            show = None
            if cls in ("DIFF", "removed-but-reachable"):
                side = "a" if sa == "reachable" else "b"
                m = ma if side == "a" else mb
                w = next((r["witness"] for r in m[k] if r.get("witness")), None)
                if w:
                    parts.append(f"witness={side}:{w}")
                    show = (side, w)
            lines.append(" ".join(parts))
            if show:
                side, w = show
                other_f, other_args = (fb, a.b) if side == "a" else (f, a.a)
                other = "b" if side == "a" else "a"
                try:
                    keys = replay(a, other_f, other_args, w, [f"--place-in={layout}"])
                    lines.append(
                        f"replay: {job} {key_text(k)} on={other} witness={w} "
                        f"reached={'yes' if k in keys else 'no'} "
                        f"keys={','.join(key_text(x) for x in keys) or '-'}"
                    )
                except SeaVcError as e:
                    lines.append(
                        f"replay: {job} {key_text(k)} on={other} witness={w} "
                        f"reached=error reason={e}"
                    )
            if cls == "equal" and sa == "reachable":
                if len(ma[k]) == 1 and len(mb[k]) == 1:
                    both_reachable.append(k)
                else:
                    copies.append(k)
        no_targets = not keys
        if no_targets:
            # nothing compared (no tagged target on either side): no pass
            reason = "no-targets"
            lines.append(f"target: {job} - unknown reason=no-targets")
        if a.verdict:
            try:
                v, _, _, _ = placed(
                    a,
                    work,
                    "verdict",
                    lambda side, pl: safety_verdict(a, *inputs[side], pl),
                    writer,
                )
                va, vb = v["a"], v["b"]
            except NoLayout as e:
                lines.append(f"layout: {job} verdict unknown reason=layout {e}")
                va = vb = "unknown"
            if {va, vb} == {"sat", "unsat"}:
                vcls = "DIFF"
            elif va == vb and va in ("sat", "unsat"):
                vcls = "equal"
            else:
                vcls = "other"
            lines.append(f"verdict: {job} {vcls} a={va} b={vb}")
            verdict_diff = vcls == "DIFF"
            # sat or unsat against unknown / inconclusive: no pass
            verdict_unknown = vcls == "other"
        for k in copies if a.level >= 1 else []:
            # R_T of one copy is not the target: not compared
            l1["unknown"] += 1
            lines.append(f"level1: {job} {key_text(k)} unknown reason=copies")
        l1_pairs = None
        if a.level >= 1 and both_reachable:
            try:
                rt, _, _, _ = placed(
                    a,
                    work,
                    "rt",
                    lambda side, pl: rt_programs(
                        a, *inputs[side], os.path.join(work, f"{side}-rt"), pl
                    ),
                    writer,
                )
                l1_pairs = rt["a"], rt["b"]
            except NoLayout as e:
                for k in both_reachable:
                    l1["unknown"] += 1
                    lines.append(
                        f"level1: {job} {key_text(k)} unknown reason=layout {e}"
                    )
        if l1_pairs:
            pa, pb = l1_pairs
            qd = query_diff()
            z3 = qd.Z3(a.z3, a.rlimit, a.timeout)
            for k in both_reachable:
                fa, fbb = pa.get(k, []), pb.get(k, [])
                if len(fa) != 1 or len(fbb) != 1:
                    l1["error"] += 1
                    lines.append(f"level1: {job} {key_text(k)} error reason=no-rt-pair")
                    continue
                (old, eo), (new, en) = smt2(a, fa[0]), smt2(a, fbb[0])
                if old is None or new is None:
                    cls, det = "error", [f"reason={eo or en}"]
                else:
                    cls, det = qd.classify(
                        old,
                        new,
                        z3,
                        project_blocks=True,
                        hints="auto" if a.hints else None,
                    )
                    if cls in ("identical", "order"):
                        det = det + ["by=identical"]
                    elif cls == "equiv-sat" and not any(
                        x.startswith("by=") for x in det
                    ):
                        det = det + ["by=names"]
                if cls in ("identical", "order", "equiv-sat"):
                    l1["equiv"] += 1
                    for x in det:
                        if x.startswith("by="):
                            l1["by-" + x[3:]] += 1
                elif cls == "DIFF":
                    l1["diff"] += 1
                elif cls == "unknown":
                    l1["unknown"] += 1
                else:
                    if cls == "both-unsat":
                        det = det + ["reason=reachable-but-unsat"]
                    l1["error"] += 1
                lines.append(" ".join([f"level1: {job} {key_text(k)} {cls}"] + det))
        if (
            counts["DIFF"]
            or counts["removed-but-reachable"]
            or l1["diff"]
            or verdict_diff
        ):
            verdict = "DIFF"
        elif l1["error"]:
            verdict = "error"
        elif counts["unknown"] or l1["unknown"] or no_targets or verdict_unknown:
            verdict = "unknown"
    except NoLayout:
        # no placement fits both sides: nothing compared, no pass
        verdict, reason = "unknown", "layout"
    except SeaVcError as e:
        verdict, reason = "error", str(e)
    except Exception as e:  # noqa: BLE001 - a job's failure is its error
        verdict, reason = (
            "error",
            f"internal:{type(e).__name__}:{e}".replace(" ", "_")[:120],
        )
    finally:
        if work and not a.keep:
            shutil.rmtree(work, ignore_errors=True)
    summary = (
        f"job: {job} {verdict} targets={sum(counts.values())} "
        f"equal={counts['equal']} diff={counts['DIFF']} "
        f"removed-ok={counts['removed-ok']} "
        f"removed-true={counts['removed-true']} "
        f"removed-but-reachable={counts['removed-but-reachable']} "
        f"unknown={counts['unknown']} untagged={untagged[0]}/{untagged[1]}"
    )
    if a.level >= 1:
        summary += (
            f" level1: equiv={l1['equiv']} diff={l1['diff']} "
            f"unknown={l1['unknown']} error={l1['error']}"
        )
    if reason:
        summary += f" reason={reason}"
    summary += f" time={time.time() - t0:.1f}"
    lines.append(summary)
    return lines, verdict, counts, l1, untagged


def main():
    if "--agent" in sys.argv:
        print(AGENT, end="")
        return 0
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        epilog=AGENT,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--sea-vc", default=shutil.which("sea-vc") or "sea-vc")
    ap.add_argument("--args", default="--lower", help="sea-vc arguments of both sides")
    ap.add_argument("--a", default="", help="sea-vc arguments of side A only")
    ap.add_argument("--b", default="", help="sea-vc arguments of side B only")
    ap.add_argument("--b-input", help="B lowers this file (one input)")
    ap.add_argument("--fuzz", type=int, default=1000, help="--reach-fuzz of each side")
    ap.add_argument("--seed", type=int, default=0, help="--reach-seed of each side")
    ap.add_argument(
        "--corpus", action="append", default=[], help="--reach-corpus of each side"
    )
    ap.add_argument("--level", type=int, choices=(0, 1), default=0)
    ap.add_argument(
        "--reach-rlimit", type=int, default=0, help="sea-vc --reach-rlimit of each side"
    )
    ap.add_argument(
        "--verdict",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="also compare the two sea-vc safety verdicts (default on)",
    )
    ap.add_argument(
        "--keep-sites",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="sea-vc --reach-keep-sites on both sides (default on): a tagged"
        " check of true becomes a reach-site marker, not erased",
    )
    ap.add_argument(
        "--unresolved-true",
        choices=("unknown", "pass"),
        default="unknown",
        help="a removed check of the constant true whose next target cannot"
        " stand for it: unknown (default) or removed-true",
    )
    ap.add_argument(
        "--rt-args",
        help="sea-vc arguments of the R_T queries (default: --seam if --args"
        " has it: R_T of a --seam lowering keeps symbolic memsets)",
    )
    ap.add_argument("--z3", default=os.environ.get("SEA_Z3", "z3"))
    ap.add_argument(
        "--hints",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="level 1: Skolem hints from the R_T block ids before the"
        " quantified check (default on)",
    )
    ap.add_argument(
        "--rlimit", type=int, default=200000000, help="z3 rlimit per level-1 check"
    )
    ap.add_argument(
        "--timeout", type=int, default=600, help="seconds per sea-vc / z3 run"
    )
    ap.add_argument("--par", type=int, default=4)
    ap.add_argument("--keep")
    a = ap.parse_args()
    a.common, a.a, a.b = shlex.split(a.args), shlex.split(a.a), shlex.split(a.b)
    for x in a.common + a.a + a.b:
        if x.startswith("--reach"):
            ap.error(f"{x}: cover-diff sets the --reach options (checks only)")
    a.rt_args = rt_args_of(a.common) if a.rt_args is None else shlex.split(a.rt_args)
    files = []
    for i in a.inputs:
        files += (
            sorted(glob.glob(os.path.join(i, "*.mlir"))) if os.path.isdir(i) else [i]
        )
    if a.b_input and len(files) != 1:
        ap.error("--b-input takes one input")
    if a.b_input:
        # a hand-written pair is matched by its own tags, never by position
        for f in files + [a.b_input]:
            if "sea.reach_site" not in open(f).read():
                ap.error(f"--b-input: {f} has no sea.reach_site tags")
    # job names: the basename, the path where two inputs share one
    base = [os.path.basename(f).removesuffix(".mlir") for f in files]
    a.names = {
        f: (b if base.count(b) == 1 else f.removesuffix(".mlir"))
        for f, b in zip(files, base)
    }
    print(
        f"config: a={shlex.join(a.a) or '-'} b={shlex.join(a.b) or '-'} "
        f"args={shlex.join(a.common) or '-'} fuzz={a.fuzz} seed={a.seed} "
        f"level={a.level} rt-args={shlex.join(a.rt_args) or '-'} "
        f"unresolved-true={a.unresolved_true} keep-sites={int(a.keep_sites)}"
        + (f" b-input={a.b_input}" if a.b_input else ""),
        flush=True,
    )
    verdicts = {"pass": 0, "DIFF": 0, "unknown": 0, "error": 0}
    totals = {
        k: 0
        for k in (
            "equal",
            "DIFF",
            "removed-ok",
            "removed-true",
            "removed-but-reachable",
            "unknown",
        )
    }
    l1t = dict.fromkeys(L1_KEYS, 0)
    untagged = [0, 0]
    with cf.ThreadPoolExecutor(a.par) as ex:
        for lines, verdict, counts, l1, ut in ex.map(lambda f: one(f, a), files):
            for line in lines:
                print(line)
            sys.stdout.flush()
            verdicts[verdict] += 1
            for k in totals:
                totals[k] += counts[k]
            for k in l1t:
                l1t[k] += l1[k]
            untagged = [untagged[0] + ut[0], untagged[1] + ut[1]]
    print(
        f"total: jobs={len(files)} pass={verdicts['pass']} diff={verdicts['DIFF']} "
        f"unknown={verdicts['unknown']} error={verdicts['error']}"
    )
    print(
        f"targets: equal={totals['equal']} diff={totals['DIFF']} "
        f"removed-ok={totals['removed-ok']} "
        f"removed-true={totals['removed-true']} "
        f"removed-but-reachable={totals['removed-but-reachable']} "
        f"unknown={totals['unknown']} untagged={untagged[0]}/{untagged[1]}"
    )
    if a.level >= 1:
        print(
            f"level1: equiv={l1t['equiv']} diff={l1t['diff']} "
            f"unknown={l1t['unknown']} error={l1t['error']} "
            f"pairs={sum(l1t[k] for k in L1_CLASSES)} "
            + " ".join(f"{k}={l1t[k]}" for k in L1_KEYS if k.startswith("by-"))
        )
    # as a job's verdict: DIFF, then error, then unknown
    if verdicts["DIFF"]:
        print("status: DIFF")
        return 1
    if verdicts["error"]:
        print("status: error")
        return 3
    if verdicts["unknown"]:
        print("status: unknown")
        return 2
    print("status: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
