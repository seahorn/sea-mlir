#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Parity of the sea-mlir BMC engine with SeaHorn's mono BMC engine.

For each SeaHorn lit test and each of its RUN lines (a configuration: the
line's tool and extra flags), runs SeaHorn with the mono engine (the
reference) and with the sea-mlir engine (export + sea-vc), both with that
line's flags, and classifies the pair. RUN lines are recognized by their
lit substitution (no mode option; test/opsem has only `%seabmc`, test/opsem2
has `%sea` and `%fpfsea`):

  %seabmc   seahorn <SEABMC> <flags> <test>            (test/opsem/lit.cfg)
  %sea      sea bpf --bmc=opsem --horn-bv2-vacuity-check=all <flags> <test>
  %fpfsea   sea fpf ... (same)                         (test/opsem2/lit.cfg)

`sea` (--sea) passes the engine flags through to seahorn; `--bmc=opsem`
makes it BMC (--horn-bmc), not CHC. `%seasmt` lines (no solving) are
ignored. Directories are not searched recursively: name each one
(test/opsem2/ownsem, ownership semantics, is out of scope).

  agree              same sat/unsat verdict (and the same per-assert check
                     reports, below)
  WRONG              different verdicts (exit status 1)
  checks-missing     same verdict, but SeaHorn printed per-assert check
                     reports (--horn-bv2-vacuity-check: "vacuity
                     passed/failed", "assertion passed/failed", which the
                     opsem2 tests CHECK) and sea-mlir printed different
                     ones (today: none; sea-mlir has no vacuity check)
  unsupported        sea-mlir stopped with an explicit error (grouped by it)
  unknown            sea-mlir gave no verdict and no error
  timeout            sea-mlir timed out
  no-reference       mono gave no verdict (error or timeout): no comparison

The vacuity check is part of mono's operational semantics (BvOpSem2.cc,
doAssert): an incremental solver call at each sea.assert.if / sea.assert,
reported on the output; it does not change the final verdict. sea-mlir's
exporter treats sea.assert.if as a no-op (a warning), so the final verdicts
are comparable, and the missing reports are counted as checks-missing,
never as agreement. sea-mlir warnings (approximations) are summarized by
kind.

Only the mono engine is a reference (SeaHorn's path engine is a prototype).
Pipeline flags apply to both engines. Flags of SeaHorn's VC encoding that
change its memory model (NOT_MODELED below) are ignored by sea-mlir: rows
with them are counted separately, since sea-mlir's verdict there does not
depend on the flag. `%fpfsea` (fat-pointer functions) is not modeled either.
Pure encoding choices (lambdas, ite, gsa, tactic) must
not change a verdict and count as modeled.

Agreement is evidence of parity on these tests and configurations only, not
on every program. The test's CHECK and XFAIL are reported when SeaHorn's own
verdict differs from them.
"""

import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

SEABMC = [
    "--horn-bmc",
    "--horn-shadow-mem-alloc-is-def",
    "--horn-bv2=true",
    "--keep-shadows=true",
    "--horn-solve",
]
# SeaHorn VC options that change its memory model; sea-mlir ignores them.
# (--horn-bv2-word-size is modeled: the engine passes it to sea-vc as
# --widen-memory, which widens only memories whose accesses provably fit,
# so the verdict does not depend on it.)
NOT_MODELED = (
    "--horn-bv2-allow-partial-word-memset",
    "--sea-opsem-allocator",
    # symbolic allocation; load-is-def shadow semantics
    "--horn-shadow-mem-load-is-def",
    "--horn-opsem-max-symb-alloc",
)
# Modeled: --horn-bv2-widemem / extra-widemem (fat pointers:
# sea-lower-fat-pointers) and --horn-bv2-tracking-mem (modification
# tracking: sea-lower-tracking); unsupported tracking features still stop
# with an explicit error.
# `sea` sub-command and fixed flags of each RUN substitution
# (test/opsem2/lit.cfg); %seabmc runs seahorn directly with SEABMC.
SEA_MODES = {"sea": "bpf", "fpfsea": "fpf"}
SEA_FLAGS = ["--bmc=opsem", "--horn-bv2-vacuity-check=all"]


def expected(text):
    m = re.search(r"CHECK:\s*\{\{\^(sat|unsat)\$\}\}", text)
    return m.group(1) if m else None


def configurations(text):
    """(mode, flags) of every `%seabmc` / `%sea` / `%fpfsea` RUN line: the
    substitution's name and the line's extra flags (without the input, the
    pipe and logging flags), in order, duplicates removed."""
    configs = []
    for line in text.splitlines():
        m = re.search(r"RUN:\s*%(seabmc|fpfsea|sea)(?![a-z])(.*)", line)
        if not m:
            continue
        cmd = m.group(2).split("|")[0]
        flags = [
            f for f in cmd.split() if f.startswith("-") and not f.startswith("--log")
        ]
        if (m.group(1), flags) not in configs:
            configs.append((m.group(1), flags))
    return configs


def checks_of(out):
    """The per-assert check reports (vacuity / incremental assertion) of a
    run, in order: e.g. "vacuity failed (unsat)"."""
    return re.findall(
        r"^(?:Error|Info): ((?:vacuity|assertion) "
        r"(?:passed|failed) \((?:sat|unsat|unknown)\))",
        out,
        re.M,
    )


def warnings_of(out):
    """sea-mlir's warnings (approximations), without run-specific parts."""
    return sorted(
        {
            re.sub(r" at: .*| at loc\(.*", "", w).strip()[:160]
            for w in re.findall(r"^Warning: (sea-mlir.*)$", out, re.M)
        }
    )


def first_problem(out):
    """The first error line, without the parts that differ between runs."""
    for line in out.splitlines():
        if "Error" in line or "error:" in line:
            line = re.sub(r'loc\("/[^"]*\.mlir":\d+:\d+\): ', "", line)
            line = re.sub(r" at: .*| at loc\(.*", "", line)
            return line.strip()[:160]
    return None


def verdict_of(args, mode, engine_flags, f):
    """(sat|unsat|None, timed out, combined output) of one seahorn run."""
    if mode == "seabmc":
        cmd = [args.seahorn] + SEABMC + engine_flags + [f]
    else:
        cmd = [args.sea, SEA_MODES[mode]] + engine_flags + SEA_FLAGS + [f]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return None, True, ""
    got = (
        "sat"
        if re.search(r"^sat$", r.stdout, re.M)
        else "unsat"
        if re.search(r"^unsat$", r.stdout, re.M)
        else None
    )
    return got, False, r.stdout + r.stderr


def compare(args, f, mode, flags):
    """Classifies one (test, configuration) pair:
    (category, detail, SeaHorn's verdict, sea-mlir's warnings)."""
    ref, ref_to, ref_out = verdict_of(args, mode, ["--horn-bmc-engine=mono"] + flags, f)
    if ref is None:
        return "no-reference", "timeout" if ref_to else "no verdict", ref, []
    got, to, out = verdict_of(
        args,
        mode,
        ["--horn-bmc-engine=sea-mlir", f"--horn-sea-vc={args.sea_vc}"] + flags,
        f,
    )
    problem = first_problem(out)
    warnings = warnings_of(out)
    if to:
        return "timeout", None, ref, warnings
    if got is not None and got != ref:
        return "WRONG", f"SeaHorn {ref}, sea-mlir {got}", ref, warnings
    if got == ref and not problem:
        ref_checks, checks = checks_of(ref_out), checks_of(out)
        if ref_checks != checks:
            return (
                "checks-missing",
                f"SeaHorn: {'; '.join(ref_checks) or 'none'}; "
                f"sea-mlir: {'; '.join(checks) or 'none'}",
                ref,
                warnings,
            )
        return "agree", None, ref, warnings
    if problem:
        return "unsupported", problem, ref, warnings
    return "unknown", None, ref, warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="+", help="test files or directories")
    ap.add_argument("--seahorn", required=True)
    ap.add_argument("--sea-vc", required=True)
    ap.add_argument(
        "--sea",
        help="SeaHorn's `sea` driver (needed for "
        "%%sea / %%fpfsea RUN lines, test/opsem2)",
    )
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4, help="parallel runs")
    ap.add_argument(
        "--first-only", action="store_true", help="only each test's first configuration"
    )
    ap.add_argument("-v", action="store_true", help="list every row")
    args = ap.parse_args()

    files = []
    for t in args.tests:
        if os.path.isdir(t):
            files += sorted(
                os.path.join(t, f) for f in os.listdir(t) if f.endswith((".ll", ".c"))
            )
        else:
            files.append(t)

    rows = []  # (file, mode, flags, modeled)
    notes = []
    skipped = 0
    texts = {}
    for f in files:
        text = texts[f] = open(f, errors="replace").read()
        configs = configurations(text)
        if not configs:
            skipped += 1
            continue
        for mode, flags in configs[:1] if args.first_only else configs:
            modeled = mode != "fpfsea" and not any(
                fl.startswith(NOT_MODELED) for fl in flags
            )
            rows.append((f, mode, flags, modeled))
    if not args.sea and any(r[1] != "seabmc" for r in rows):
        ap.error("%sea / %fpfsea RUN lines need --sea")

    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        results = list(pool.map(lambda r: compare(args, r[0], r[1], r[2]), rows))

    counts = {True: collections.Counter(), False: collections.Counter()}
    problems = collections.defaultdict(list)
    ref_verdicts = collections.defaultdict(set)
    warned = collections.defaultdict(list)
    for (f, mode, flags, modeled), (cat, detail, ref, warnings) in zip(rows, results):
        counts[modeled][cat] += 1
        name = os.path.basename(f)
        if ref:
            ref_verdicts[f].add(ref)
        if cat in ("WRONG", "unsupported", "checks-missing"):
            problems[(cat, detail)].append(name)
        for w in warnings:
            warned[w].append(name)
        if args.v:
            tool = "" if mode == "seabmc" else f"%{mode} "
            print(
                f"{name} [{tool}{' '.join(flags) or '-'}]: {cat}"
                f"{': ' + detail if detail else ''}"
            )
    for f, refs in ref_verdicts.items():
        exp = expected(texts[f])
        xfail = " (XFAIL)" if "XFAIL:" in texts[f] else ""
        if len(refs) > 1:
            notes.append(
                f"{os.path.basename(f)}: SeaHorn's verdict depends "
                f"on the configuration: {sorted(refs)}{xfail}"
            )
        elif exp and refs != {exp}:
            notes.append(
                f"{os.path.basename(f)}: SeaHorn says "
                f"{next(iter(refs))}, CHECK says {exp}{xfail}"
            )

    print(
        "\n== parity with SeaHorn mono "
        f"({len(files) - skipped} tests, {len(rows)} configurations; "
        f"{skipped} tests without a %seabmc/%sea/%fpfsea RUN line)"
    )
    cats = [
        "agree",
        "WRONG",
        "checks-missing",
        "unsupported",
        "unknown",
        "timeout",
        "no-reference",
    ]
    for modeled, title in (
        (True, "modeled configurations"),
        (
            False,
            "configurations with flags sea-mlir does "
            "not model (" + ", ".join(NOT_MODELED) + ", %fpfsea)",
        ),
    ):
        c = counts[modeled]
        if not sum(c.values()):
            continue
        print(f"{title}: " + ", ".join(f"{k} {c[k]}" for k in cats if c[k]))
    if notes:
        print("\n== SeaHorn's reference itself")
        for n in notes:
            print("  " + n)
    print("\n== WRONG, checks-missing and unsupported, by cause (most frequent first)")
    for (cat, detail), names in sorted(problems.items(), key=lambda kv: -len(kv[1])):
        uniq = sorted(set(names))
        print(f"{len(names):4d}  {cat}: {detail}")
        print("        e.g. " + ", ".join(uniq[:3]))
    if warned:
        print("\n== sea-mlir warnings (approximations), by kind")
        for w, names in sorted(warned.items(), key=lambda kv: -len(kv[1])):
            print(f"{len(names):4d}  {w}")
            print("        e.g. " + ", ".join(sorted(set(names))[:3]))
    sys.exit(1 if counts[True]["WRONG"] + counts[False]["WRONG"] else 0)


if __name__ == "__main__":
    main()
