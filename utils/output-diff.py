#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""output-diff: compare the output of two builds of sea-vc / sea-run over a
corpus and an argument matrix.

The gate of a change that must not change what the tools print (a timer,
statistics or solver refactoring): every job runs OLD and NEW with the same
arguments, and stdout, stderr and the exit status must agree after the
timings and memory sizes are masked. Per job, the multiset of stat keys
(`key: ...` lines) and of BRUNCH_STAT keys is compared too, so a stat line
that moves, appears or disappears is named.
"""

import argparse
import collections
import concurrent.futures as cf
import difflib
import glob
import importlib.util
import json
import os
import re
import subprocess
import sys
import time

AGENT = """\
output-diff: compare what two builds of sea-vc / sea-run print, over a
corpus x an argument matrix x the output formats (text, jsonl).
WHY: the gate of a refactoring that must not change output (timers, the
  stats registry, the solver factory): every job must be identical after
  masking timings and memory sizes, and keep its stat and BRUNCH_STAT keys.
USAGE:
  utils/output-diff.py --old <bin-dir> --new <bin-dir> [inputs...]
      [--quick] [--par 2] [--timeout 300] [--jobs REGEX] [--sample N]
      [--exclude REGEX (default ^VC/limits/: the --timeout tests)]
      [--matrix FILE] [--entry 'NAME TOOL [FLAGS]: ARGS']... [--select A,B]
      [--formats text,jsonl] [--vc-common ARGS] [--run-common ARGS]
      [--keep DIR] [--quiet] [--print-matrix]
  utils/output-diff.py --self-test     (the masks and the key lists; no build)
  <bin-dir> holds sea-vc and sea-run (OLD = NEW: the noise floor).
CORPUS (no inputs): $SEA_BENCH_SUITES/vcc, and the standalone programs of
  test/VC and test/Interp (no split markers). A directory is searched
  recursively for *.mlir; files with `// -----` or `//---` are skipped.
  Every input runs every matrix entry.
MATRIX: one entry per line, `NAME TOOL [FLAGS]: ARGS` (# comments);
  TOOL sea-vc | sea-run; FLAGS: heavy (skipped by --quick), text / jsonl
  (only that format). --vc-common / --run-common are appended to every
  entry of that tool (default sea-vc: --log=vc.phase,lower.pass,vc.solve).
  Before any job, every --flag of the matrix must be an option of both
  binaries (--help-hidden), else status 2: a flag one side does not know
  would make both sides fail alike and compare identical.
MASKS (both sides; utils/jsonl-check.py's masks plus these):
  <name>time[:] <d.ddd>, time= execs/s= steps/s=, rss-mb= vm-mb= peak-rss-mb=,
  slowest-op[-at]=, `<key>-mb[:] <n>`, the tool.version line, the bin dir,
  the load address of a crash backtrace frame (` #N 0x...`);
  JSON-object lines (jsonl stdout, jsonl log events on stderr): every
  string field masked as text, a number field whose name
  contains `time` (decimals only: timeout stays) or ends in -mb / _mb, and
  the value of a stat record whose key does. --smt2 jobs: stdout unmasked.
OUTPUT (stdout):
  <job> <entry> <format> identical [rc=<n>]
  <job> <entry> <format> DIFF [stdout] [stderr] [rc=<old>/<new>]
      [keys=+k,-k] [brunch=+k,-k]  then indented first differing lines
  <job> <entry> <format> timeout side=old (new not run)|new | error why=..
  entry: <name> tool=<t> jobs=<n> identical=<n> diff=<n> rc-nonzero=<n>
      secs=<run time of both sides>
  total: jobs=<n> identical=<n> diff=<n> keys-diff=<n> brunch-diff=<n>
      timeout=<n> error=<n>
  skipped: <entry>,... (--quick)   [only with --quick]
  wall: <s>
  status: pass | fail | incomplete
  rc-nonzero counts jobs that failed alike on both sides: an entry whose
  jobs all fail checks nothing.
STATUS: 0 pass (every job identical); 1 some job DIFF; 2 a timeout, an
  error or a bad matrix (checked in this order).
--keep DIR writes both sides' stdout / stderr of every non-identical job.
"""

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)


def _load_jsonl_check():
    spec = importlib.util.spec_from_file_location(
        "jsonl_check", os.path.join(HERE, "jsonl-check.py")
    )
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


jc = _load_jsonl_check()

V = " --lower --widen-memory=8"
SEAM = V + " --seam --seam-stats --brunch-stats"
# The plan's matrix (stage 0). `heavy` (skipped by --quick): extra solver checks,
# the plain array encoding (default, lambda) and the read-table experiment (xrt):
# minutes per vcc job, some past the timeout.
DEFAULT_MATRIX = f"""\
default sea-vc heavy:{V}
seam-stats sea-vc:{SEAM}
lambda sea-vc heavy:{V} --mem=lambda --brunch-stats
xrt sea-vc heavy:{SEAM} --x-read-table=mux
xrt-check sea-vc heavy:{SEAM} --x-read-table=mux --x-read-table-check=1
words sea-vc heavy:{SEAM} --seam-words=auto --words-check
eq-audit sea-vc heavy:{SEAM} --eq-audit
split-auto sea-vc heavy:{SEAM} --split-auto
reach sea-vc heavy:{V} --reach --brunch-stats
smt2 sea-vc text:{V} --seam --smt2
run sea-run: --missing=zero
fuzz sea-run: --fuzz=200 --seed=1
"""

MB_LINE = re.compile(r"(\S*-mb:? )[0-9]+")
VERSION = re.compile(r"^(tool\.version: ).*$", re.M)
# a frame of LLVM's crash backtrace: the load address differs per run (ASLR)
FRAME = re.compile(r"^( *#[0-9]+ )0x[0-9a-f]+", re.M)
KEY = re.compile(r"^([A-Za-z_][\w.\-/]*):(?: |$)")
BRUNCH = re.compile(r"^BRUNCH_STAT (\S+)")
DECIMAL = re.compile(r"-?[0-9]+\.[0-9]+$")
TOOLS = ("sea-vc", "sea-run")


# ------------------------------------------------------------- normalization
def mask_text(s, bindir=None):
    """Text with timings, memory sizes and the version masked."""
    if bindir:
        s = s.replace(bindir.rstrip("/") + "/", "<bin>/")
    s = jc.mask(s)
    s = MB_LINE.sub(r"\1M", s)
    s = FRAME.sub(r"\1ADDR", s)
    return VERSION.sub(r"\1V", s)


def _timeish(name):
    return "time" in name


def _mbish(name):
    return name.endswith("-mb") or name.endswith("_mb")


def mask_json(x, bindir=None):
    """A parsed jsonl record (numbers kept as strings) with timings masked."""
    if isinstance(x, list):
        return [mask_json(v, bindir) for v in x]
    if not isinstance(x, dict):
        return mask_text(x, bindir) if isinstance(x, str) else x
    key = x.get("key") if isinstance(x.get("key"), str) else ""
    out = {}
    for k, v in x.items():
        if isinstance(v, (dict, list)):
            out[k] = mask_json(v, bindir)
        elif k == "value" and key == "tool.version":
            out[k] = "V"
        elif isinstance(v, str) and DECIMAL.match(v) and _timeish(k):
            out[k] = "T"
        elif isinstance(v, str) and v.isdigit() and _mbish(k):
            out[k] = "M"
        elif k == "value" and isinstance(v, str) and DECIMAL.match(v) and _timeish(key):
            out[k] = "T"
        elif k == "value" and isinstance(v, str) and v.isdigit() and _mbish(key):
            out[k] = "M"
        elif isinstance(v, str):
            out[k] = mask_text(v, bindir)
        else:
            out[k] = v
    return out


def mask_lines(text, bindir):
    """Text with every JSON-object line masked as a record, the other
    lines as text (jsonl stdout, and the jsonl log events on stderr)."""
    lines = []
    for line in text.splitlines():
        if line.startswith("{"):
            try:
                r = json.loads(line, parse_float=str, parse_int=str)
            except json.JSONDecodeError:
                r = None
            if isinstance(r, dict):
                lines.append(json.dumps(mask_json(r, bindir), separators=(",", ":")))
                continue
        lines.append(mask_text(line, bindir))
    return "".join(x + "\n" for x in lines)


def normalize(stdout, stderr, smt2, bindir):
    """(stdout, stderr) as compared."""
    return (stdout if smt2 else mask_lines(stdout, bindir)), mask_lines(stderr, bindir)


def stdout_texts(stdout, fmt):
    """The text lines of stdout: jsonl records' `text` fields (split at
    newlines), other lines as they are."""
    if fmt != "jsonl":
        return stdout.splitlines()
    out = []
    for line in stdout.splitlines():
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            out.append(line)
            continue
        if isinstance(r, dict) and isinstance(r.get("text"), str):
            out += r["text"].split("\n")
        else:
            out.append(line)
    return out


def keys(stdout, fmt):
    """(Counter of stat keys, Counter of BRUNCH_STAT keys) of stdout."""
    stat, brunch = collections.Counter(), collections.Counter()
    for line in stdout_texts(stdout, fmt):
        m = BRUNCH.match(line)
        if m:
            brunch[m.group(1)] += 1
            continue
        m = KEY.match(line)
        if m:
            stat[m.group(1)] += 1
    return stat, brunch


def key_diff(old, new):
    """`+k` for keys NEW has more often, `-k` for keys OLD has more often."""
    d = [f"+{k}" for k in sorted(new) if new[k] > old.get(k, 0)]
    d += [f"-{k}" for k in sorted(old) if old[k] > new.get(k, 0)]
    return d


def first_diff(a, b, n=3):
    """The first differing lines of a and b (unified diff, n lines)."""
    out = []
    for line in difflib.unified_diff(
        a.splitlines(), b.splitlines(), "old", "new", n=0, lineterm=""
    ):
        if line.startswith(("---", "+++")):
            continue
        out.append(line)
        if len(out) >= n * 2 + 1:
            break
    return out


# ------------------------------------------------------------------- matrix
class Entry:
    def __init__(self, name, tool, flags, args):
        self.name, self.tool, self.flags, self.args = name, tool, flags, args

    @property
    def heavy(self):
        return "heavy" in self.flags

    def formats(self, formats):
        only = [f for f in ("text", "jsonl") if f in self.flags]
        return [f for f in formats if not only or f in only]

    def __str__(self):
        head = " ".join([self.name, self.tool] + self.flags)
        return f"{head}: {' '.join(self.args)}"


def parse_entry(line):
    head, sep, args = line.partition(":")
    words = head.split()
    if not sep or len(words) < 2:
        raise ValueError(f"matrix entry {line!r}: expected `NAME TOOL [FLAGS]: ARGS`")
    name, tool, flags = words[0], words[1], words[2:]
    if tool not in TOOLS:
        raise ValueError(f"matrix entry {name}: tool {tool!r} is not one of {TOOLS}")
    bad = [f for f in flags if f not in ("heavy", "text", "jsonl")]
    if bad:
        raise ValueError(f"matrix entry {name}: unknown flag(s) {bad}")
    return Entry(name, tool, flags, args.split())


def parse_matrix(text):
    out = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(parse_entry(line))
    names = [e.name for e in out]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        raise ValueError(f"matrix: duplicate entry names {dup}")
    return out


HELP_OPT = re.compile(r"^\s+--([A-Za-z0-9][\w-]*)", re.M)


def known_options(binary):
    p = subprocess.run(
        [binary, "--help-hidden"], capture_output=True, text=True, timeout=60
    )
    return set(HELP_OPT.findall(p.stdout + p.stderr))


def check_flags(matrix, sides, common):
    """Messages for the matrix flags some binary does not accept."""
    errors = []
    for tool in TOOLS:
        entries = [e for e in matrix if e.tool == tool]
        if not entries:
            continue
        for side, bindir in sides:
            path = os.path.join(bindir, tool)
            if not os.access(path, os.X_OK):
                errors.append(f"{side}: no executable {path}")
                continue
            opts = known_options(path)
            for e in entries:
                for a in e.args + common[tool]:
                    if not a.startswith("--"):
                        continue
                    name = a[2:].split("=", 1)[0]
                    if name not in opts:
                        errors.append(
                            f"{side}: {tool} does not accept --{name} (entry {e.name})"
                        )
    return sorted(set(errors))


# ------------------------------------------------------------------- corpus
SPLIT = re.compile(r"^//( -----|---)", re.M)


def standalone(path):
    try:
        text = open(path, errors="replace").read()
    except OSError:
        return False
    return "sea.program" in text and not SPLIT.search(text)


def collect(inputs):
    """[(job name, path)] of the inputs: files as given, directories
    recursively (standalone programs only)."""
    jobs = []
    for i in inputs:
        if os.path.isdir(i):
            root = os.path.abspath(i)
            tag = os.path.basename(root.rstrip("/"))
            for f in sorted(
                glob.glob(os.path.join(root, "**", "*.mlir"), recursive=True)
            ):
                if standalone(f):
                    rel = os.path.relpath(f, root)[: -len(".mlir")]
                    jobs.append((f"{tag}/{rel}", f))
        else:
            jobs.append((os.path.basename(i).removesuffix(".mlir"), os.path.abspath(i)))
    return jobs


def default_inputs():
    suites = os.environ.get("SEA_BENCH_SUITES")
    vcc = os.path.join(suites, "vcc") if suites else None
    if not vcc or not os.path.isdir(vcc):
        return None
    return [vcc, os.path.join(SRC, "test", "VC"), os.path.join(SRC, "test", "Interp")]


# --------------------------------------------------------------------- jobs
def run_side(bindir, tool, path, args, timeout):
    """(rc, stdout, stderr, seconds) or (None, ..) on a timeout."""
    t = time.monotonic()
    try:
        p = subprocess.run(
            [os.path.join(bindir, tool), path] + args,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            stdin=subprocess.DEVNULL,
        )
    except subprocess.TimeoutExpired:
        return None, "", "", time.monotonic() - t
    except OSError as e:
        return "error", "", str(e), time.monotonic() - t
    return p.returncode, p.stdout, p.stderr, time.monotonic() - t


def one(task, a):
    (job, path), entry, fmt = task
    args = entry.args + a.common[entry.tool] + [f"--format={fmt}"]
    smt2 = "--smt2" in entry.args
    sides = {}
    sides["old"] = run_side(a.old, entry.tool, path, args, a.timeout)
    if sides["old"][0] is None:
        # nothing to compare: do not spend another timeout on NEW
        sides["new"] = ("skipped", "", "", 0.0)
    else:
        sides["new"] = run_side(a.new, entry.tool, path, args, a.timeout)
    (rc_o, out_o, err_o, t_o), (rc_n, out_n, err_n, t_n) = sides["old"], sides["new"]
    r = {"job": job, "entry": entry, "fmt": fmt, "details": [], "lines": []}
    r["secs"] = (t_o, t_n)
    if rc_o != "skipped" and "error" in (rc_o, rc_n):
        r["cls"] = "error"
        r["details"] = [
            "why=" + (err_o if rc_o == "error" else err_n).replace(" ", "_")
        ]
        return r
    if rc_o is None or rc_n is None:
        r["cls"] = "timeout"
        r["details"] = ["side=old (new not run)" if rc_o is None else "side=new"]
        return r
    no, eo = normalize(out_o, err_o, smt2, a.old)
    nn, en = normalize(out_n, err_n, smt2, a.new)
    ko, bo = keys(out_o, fmt)
    kn, bn = keys(out_n, fmt)
    kd, bd = key_diff(ko, kn), key_diff(bo, bn)
    r["keys_diff"], r["brunch_diff"] = bool(kd), bool(bd)
    r["rc"] = rc_o
    if no == nn and eo == en and rc_o == rc_n and not kd and not bd:
        r["cls"] = "identical"
        if rc_o:
            r["details"] = [f"rc={rc_o}"]
        return r
    r["cls"] = "DIFF"
    d = r["details"]
    if no != nn:
        d.append("stdout")
    if eo != en:
        d.append("stderr")
    if rc_o != rc_n:
        d.append(f"rc={rc_o}/{rc_n}")
    if kd:
        d.append("keys=" + ",".join(kd))
    if bd:
        d.append("brunch=" + ",".join(bd))
    if no != nn:
        r["lines"] += ["stdout " + x for x in first_diff(no, nn)]
    if eo != en:
        r["lines"] += ["stderr " + x for x in first_diff(eo, en)]
    if a.keep:
        base = os.path.join(a.keep, f"{job}.{entry.name}.{fmt}".replace("/", "__"))
        os.makedirs(a.keep, exist_ok=True)
        for tag, text in (
            ("old.out", out_o),
            ("old.err", err_o),
            ("new.out", out_n),
            ("new.err", err_n),
        ):
            with open(f"{base}.{tag}", "w") as fh:
                fh.write(text)
    return r


# ---------------------------------------------------------------- self-test
def self_test():
    ok = True

    def check(what, got, want):
        nonlocal ok
        if got != want:
            ok = False
            print(f"self-test: FAIL {what}: got {got!r} want {want!r}")

    # text masks: the jsonl-check ones, -mb keys, the version, the bin dir
    t1 = (
        "tool.version: sea-mlir 0.1.0 (abc)\nvc-time: 0.061\n"
        "log: vc.phase end phase=lower time=0.002 rss-mb=45 vm-mb=88 peak-rss-mb=45\n"
        "BRUNCH_STAT vc-time 0.061\nfuzz.trials: 50 time=0.003 execs/s=19129\n"
        "solver.peak-mb: 120\ntimeout: 300\n/x/old/sea-vc: error\n"
    )
    t2 = t1.replace("abc", "def").replace("0.061", "1.500").replace("45", "46")
    t2 = t2.replace("19129", "7").replace("120", "99").replace("/x/old/", "/y/new/")
    check("text mask", mask_text(t1, "/x/old"), mask_text(t2, "/y/new"))
    check("timeout kept", "timeout: 300" in mask_text(t1), True)
    check("count kept", mask_text("vc.reads: 12"), "vc.reads: 12")
    f1 = " #0 0x00005aa9d1551a1b llvm::sys::PrintStackTrace() (sea-vc+0x8f5a1b)"
    f2 = f1.replace("5aa9d1551a1b", "5937c34c6a1b")
    check("crash frame", mask_text(f1), mask_text(f2))
    check(
        "frame offset kept",
        mask_text(f1) != mask_text(f1.replace("8f5a1b", "8f5a1c")),
        True,
    )
    # jsonl: string fields, time-named decimals, stat values by key
    j1 = (
        '{"kind":"stat","key":"tool.version","value":"sea-mlir 0.1.0 (a)",'
        '"text":"tool.version: sea-mlir 0.1.0 (a)"}\n'
        '{"kind":"stat","key":"vc-time","value":0.061,"text":"vc-time: 0.061"}\n'
        '{"kind":"result","value":"unsat","vc_time":0.5,"timeout":300,"text":"result: unsat"}\n'
        '{"kind":"stat","key":"x-mb","value":12,"text":"x-mb: 12"}\n'
    )
    j2 = j1.replace("(a)", "(b)").replace("0.061", "0.07").replace("0.5", "0.25")
    j2 = j2.replace("12", "13")
    check(
        "jsonl mask",
        normalize(j1, "", False, None),
        normalize(j2, "", False, None),
    )
    j3 = j1.replace('"timeout":300', '"timeout":301')
    check(
        "jsonl timeout kept",
        normalize(j1, "", False, None) != normalize(j3, "", False, None),
        True,
    )
    # jsonl log events on stderr
    e1 = '{"log":"vc.phase","time":0.002,"rss-mb":14,"ops":8,"text":"log: time=0.002"}'
    e2 = e1.replace("0.002", "0.004").replace("14", "15")
    check(
        "jsonl stderr", normalize("", e1, False, None), normalize("", e2, False, None)
    )
    e3 = e1.replace('"ops":8', '"ops":9')
    check(
        "jsonl count kept",
        normalize("", e1, False, None) != normalize("", e3, False, None),
        True,
    )
    # smt2: stdout unmasked
    s = "; time 0.5\n(check-sat)\n"
    check("smt2 raw", normalize(s, "", True, None)[0], s)
    # keys, text and jsonl alike
    txt = "program: @main\nvc.reads: 3\nBRUNCH_STAT vc.reads 3\nobject: a\nobject: b\n"
    k, b = keys(txt, "text")
    check(
        "text keys",
        (dict(k), dict(b)),
        ({"program": 1, "vc.reads": 1, "object": 2}, {"vc.reads": 1}),
    )
    js = "".join(
        json.dumps({"kind": "stat", "text": x}) + "\n" for x in txt.splitlines()
    )
    check("jsonl keys", keys(js, "jsonl"), (k, b))
    k2, _ = keys(txt + "extra.key: 1\n", "text")
    check("key diff", key_diff(k, k2), ["+extra.key"])
    check("key count diff", key_diff(k2, k), ["-extra.key"])
    # the matrix
    m = parse_matrix(DEFAULT_MATRIX)
    check("matrix tools", {e.tool for e in m}, set(TOOLS))
    check(
        "smt2 text only",
        [e.formats(["text", "jsonl"]) for e in m if e.name == "smt2"],
        [["text"]],
    )
    for bad in ("x sea-vc", "x sea-xx: --a", "x sea-vc odd: --a"):
        try:
            parse_entry(bad)
            check(f"reject {bad!r}", "accepted", "ValueError")
        except ValueError:
            pass
    print("self-test: pass" if ok else "self-test: fail")
    return 0 if ok else 1


# --------------------------------------------------------------------- main
def main():
    if "--agent" in sys.argv:
        print(AGENT, end="")
        return 0
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--old", help="bin directory of the OLD build")
    ap.add_argument("--new", help="bin directory of the NEW build")
    ap.add_argument("inputs", nargs="*")
    ap.add_argument("--matrix", help="matrix file (default: the built-in matrix)")
    ap.add_argument(
        "--entry",
        action="append",
        default=[],
        help="a matrix entry `NAME TOOL [FLAGS]: ARGS` (repeatable; replaces the matrix)",
    )
    ap.add_argument("--select", help="comma-separated entry names to run")
    ap.add_argument("--quick", action="store_true", help="skip the heavy entries")
    ap.add_argument("--formats", default="text,jsonl")
    ap.add_argument("--vc-common", default="--log=vc.phase,lower.pass,vc.solve")
    ap.add_argument("--run-common", default="")
    ap.add_argument("--par", type=int, default=2, help="jobs at once (default: 2)")
    ap.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="seconds per run of one side (default: %(default)s)",
    )
    ap.add_argument("--jobs", help="regex: only the inputs whose job name matches")
    ap.add_argument(
        "--exclude",
        default=r"^VC/limits/",
        help="regex: skip the inputs whose job name matches (default: %(default)s,"
        " the --timeout tests, which run for minutes without one)",
    )
    ap.add_argument("--sample", type=int, default=0, help="every N-th input only")
    ap.add_argument(
        "--keep", help="write both sides' output of non-identical jobs here"
    )
    ap.add_argument("--quiet", action="store_true", help="no lines for identical jobs")
    ap.add_argument("--print-matrix", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    try:
        if a.entry:
            matrix = parse_matrix("\n".join(a.entry))
        elif a.matrix:
            matrix = parse_matrix(open(a.matrix).read())
        else:
            matrix = parse_matrix(DEFAULT_MATRIX)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if a.select:
        want = a.select.split(",")
        missing = [n for n in want if n not in {e.name for e in matrix}]
        if missing:
            print(f"error: --select: no entries {missing}", file=sys.stderr)
            return 2
        matrix = [e for e in matrix if e.name in want]
    skipped = [e.name for e in matrix if a.quick and e.heavy]
    matrix = [e for e in matrix if not (a.quick and e.heavy)]
    if a.print_matrix:
        for e in matrix:
            print(f"matrix: {e}")
        return 0
    if not a.old or not a.new:
        print("error: --old and --new are required", file=sys.stderr)
        return 2
    a.old, a.new = os.path.abspath(a.old), os.path.abspath(a.new)
    a.common = {"sea-vc": a.vc_common.split(), "sea-run": a.run_common.split()}
    formats = [f for f in a.formats.split(",") if f]
    if not formats or any(f not in ("text", "jsonl") for f in formats):
        print(f"error: --formats {a.formats}: text and/or jsonl", file=sys.stderr)
        return 2
    inputs = a.inputs or default_inputs()
    if not inputs:
        print(
            "error: no inputs and no $SEA_BENCH_SUITES/vcc (just bench-connect)",
            file=sys.stderr,
        )
        return 2
    jobs = collect(inputs)
    if a.jobs:
        jobs = [j for j in jobs if re.search(a.jobs, j[0])]
    if a.exclude:
        jobs = [j for j in jobs if not re.search(a.exclude, j[0])]
    if a.sample > 1:
        jobs = jobs[:: a.sample]
    if not jobs:
        print("error: no inputs selected", file=sys.stderr)
        return 2
    errors = check_flags(matrix, (("old", a.old), ("new", a.new)), a.common)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        return 2
    print(
        f"output-diff: old={a.old} new={a.new} inputs={len(jobs)} "
        f"entries={len(matrix)} formats={','.join(formats)} par={a.par}",
        flush=True,
    )
    tasks = [(j, e, f) for e in matrix for j in jobs for f in e.formats(formats)]
    t0 = time.monotonic()
    per = {e.name: collections.Counter() for e in matrix}
    tot = collections.Counter()
    with cf.ThreadPoolExecutor(a.par) as ex:
        for r in ex.map(lambda t: one(t, a), tasks):
            e = r["entry"]
            c = per[e.name]
            c["jobs"] += 1
            c["secs"] += sum(r["secs"])
            c[r["cls"]] += 1
            tot["jobs"] += 1
            tot[r["cls"]] += 1
            c["rc-nonzero"] += r["cls"] == "identical" and bool(r.get("rc"))
            tot["keys-diff"] += bool(r.get("keys_diff"))
            tot["brunch-diff"] += bool(r.get("brunch_diff"))
            if r["cls"] == "identical" and a.quiet:
                continue
            print(" ".join([r["job"], e.name, r["fmt"], r["cls"]] + r["details"]))
            for x in r["lines"]:
                print("  " + x)
            sys.stdout.flush()
    for e in matrix:
        c = per[e.name]
        extra = "".join(f" {k}={c[k]}" for k in ("timeout", "error") if c[k])
        print(
            f"entry: {e.name} tool={e.tool} jobs={c['jobs']} identical={c['identical']} "
            f"diff={c['DIFF']} rc-nonzero={c['rc-nonzero']}{extra} "
            f"secs={c['secs']:.0f}"
        )
    print(
        f"total: jobs={tot['jobs']} identical={tot['identical']} diff={tot['DIFF']} "
        f"keys-diff={tot['keys-diff']} brunch-diff={tot['brunch-diff']} "
        f"timeout={tot['timeout']} error={tot['error']}"
    )
    if a.quick:
        print("skipped: " + (",".join(skipped) or "-"))
    print(f"wall: {time.monotonic() - t0:.0f}")
    if tot["DIFF"]:
        status, rc = "fail", 1
    elif tot["timeout"] or tot["error"]:
        status, rc = "incomplete", 2
    else:
        status, rc = "pass", 0
    print(f"status: {status}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
