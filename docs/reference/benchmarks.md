# Verifier benchmarks and gates

This suite gates changes to **Sea-MLIR** by running fixed exported programs
and checking their verdicts, replay results and performance counters. It
does not regenerate jobs from new application source. To gate changes to
an application, use the [application verification workflow](../agents/index.md)
and regenerate its jobs before solving.

The regression tests (lit, `test/`) are in the sea-mlir repository. The
benchmarks are not: they are large and carry third-party code under its
own licenses. They live in their own repository,
[seahorn/sea-mlir-bench](https://github.com/seahorn/sea-mlir-bench), and
are connected to a sea-mlir checkout on demand. `utils/bench.py` runs them;
`python3 utils/bench.py --agent` is its terse reference.

## The suites

| family | contents | default arguments |
|---|---|---|
| `vcc` | the aws-c-common proof harnesses | `--lower --seam --widen-memory=8` |
| `firedancer` | FD-FRAG, TX-PARSE, HPACK, HTTP/2 flow control | `--lower --seam --widen-memory=4` |
| `stellar` | C ports of stellar-core components | `--lower --seam --widen-memory=4` |
| `nki` | NKI kernels: gather, dynamic-shape add, scatter-add | `--lower --seam` |
| `crafted` | array programs adapted from SV-COMP tasks | `--lower --seam --widen-memory=4` |

```console
$ python3 utils/bench.py suites
suite: public ...
family: public/crafted jobs=20 smoke=20 quick=0 full=0 hard=0 unsat=20 args=--lower --seam --widen-memory=4 ...
family: public/firedancer jobs=120 smoke=35 quick=29 full=23 hard=33 sat=62 unknown-ok=33 unsat=25 args=--lower --seam --widen-memory=4 ...
family: public/nki jobs=356 smoke=324 quick=25 full=3 hard=4 sat=204 unknown-ok=4 unsat=148 args=--lower --seam ...
family: public/stellar jobs=119 smoke=103 quick=11 full=5 hard=0 sat=38 unsat=81 args=--lower --seam --widen-memory=4 ...
family: public/vcc jobs=228 smoke=222 quick=6 full=0 hard=0 sat=1 unsat=227 args=--lower --seam --widen-memory=8 ...
```

## Connecting

```bash
just bench-connect        # python3 utils/bench.py connect
```

reads `bench/sources.yaml`, overlaid by `bench/sources.local.yaml` (per
machine, not committed) and by `SEA_BENCH_SOURCE_<NAME>=<url-or-path>`.
Each source becomes `bench/work/<name>`: a URL is cloned (and later pulled
fast-forward), a local path is linked. `SEA_BENCH_SUITES=<dir>:<dir>`
bypasses `bench/work/`.

## Layout and manifest

```text
<family>/<job>.mlir        one verification task
<family>/manifest.yaml     defaults, expected verdicts, tiers, counter bounds
<family>/PROVENANCE.md     sources, export commands, licenses
<family>/harness/          harness sources (C families)
```

A manifest job:

```yaml
frag-reach-busy-K1:
  expected: unknown-ok        # sat | unsat | unknown-ok
  tier: hard                  # smoke <5 s | quick <60 s | full <900 s | hard (tracked, not gated)
  time: 204.91                # wall seconds when established
  harness-expected: sat       # the harness author's expectation, if stated
  tags: [reach]               # proof | reach | mutant | blowup-regression | inb-regression | canary
  args: [--ptr-mode=address]  # appended to the family's default arguments
  replay: impossible          # a sat of this job may not replay; the only value
```

`replay: impossible` is for a `sat` job whose model the interpreter cannot
run: the program updates memory without `inplace`, so `sea-vc` prints
`result: sat`, then the error `vc.replay-impossible`, and exits with
status 1. The verdict is `sat`; the gate fails on such a run unless the
job carries the field.

The family's `defaults` give the configuration the verdicts were
established with: arguments, a timeout of 900 s, and a memory cap of
16 GB (`RLIMIT_AS`). A job that needs more is `hard` with expected
verdict `unknown-ok`; the cap is not raised for it.

## Counter bounds

A verdict says nothing about how the answer was reached. A change that
makes SeaM miss an equal address keeps every verdict and only gives the
solver more work. Counter bounds make such a change visible:

```yaml
defaults:
  counters: {vc.ackermann-constraints: {max: 100}}   # every job of the family
counter-rules:                                        # jobs whose name matches the regex
- jobs: ^sweep-gather-.*-(good|nopre)$
  counters:
    vc.ackermann-constraints: {max: 0}
  why: "design promise: SeaM resolves every read of gather"
jobs:
  sweep-scatter-N2-D2-K1-pmax2-fmax1-good:
    counters:
      vc.ackermann-constraints: {max: 52, seeded: ...}
```

- A key is any statistic `sea-vc` prints: a `BRUNCH_STAT` key (the gate
  runs with `--brunch-stats`) or a `key: value` line of stdout. The
  `seam.rule.*` counts need `--seam-stats` in the job's `args`. A bound is
  `max`, `min` or `exact`.
- A key `<line>.<field>` names one entry of a `<line>: k=v k=v ...` line,
  such as `vc.box-all.expanded` or `vc.expand-memmove.kept-over-k`: the form
  the lowering chose. A job whose output has no such line has the counter
  missing. An exact key (`BRUNCH_STAT` or `key: value`) takes precedence,
  and the first matching line counts.
- The bounds of a job are `defaults.counters`, then each matching
  `counter-rules` entry in order, then the job's own `counters`. A key set
  to `null` removes an inherited bound.
- A bound with `seeded` was recorded from an observation by
  `establish --record-counters`. A bound without it is deliberate, carries
  a `why`, and is never changed by `establish`.
- The gate checks the bounds of every job that answered as expected in
  the configuration `default`. A violation fails the gate and is reported
  apart from verdict failures. A bounded counter that is missing from the
  output is a violation, so a renamed statistic cannot pass silently.

## The gate

```text
python3 utils/bench.py gate <tier> [--config default[,default+CFG,CFG...]] [--suite SEL]...
    [--jobs REGEX] [--tag T] [--par N] [--sea-vc PATH] [--no-counters] [--no-record]
```

A gate runs every job of its tier and the lower tiers (`hard` only with
`gate hard`), in each configuration: `default` is the manifest's
defaults; `default+CFG` adds the arguments and solver of
`bench/configs/CFG.yaml`; `CFG` alone replaces the defaults. A job's
timeout is the configuration's, capped by its tier (smoke 60 s, quick
300 s).

The gate fails (exit status 1) on:

- a verdict other than the expected one for a `sat` or `unsat` job
  (`WRONG` for a flipped verdict; a timeout, unknown or error counts too);
- a replay mismatch (`sea-vc` exit status 2);
- a `sat` whose model cannot be replayed (`replay-impossible`) on a job
  without `replay: impossible`;
- an error or an in-bounds violation on a gated job, `unknown-ok` jobs
  included (a memory-out or timeout there is an allowed unknown);
- two configurations that answer `sat` and `unsat` on the same job
  (`DISAGREE-SAT-UNSAT`);
- a counter outside its bound (`FAIL: counter ...`).

It reports without failing `slow:` (more than twice the manifest time and
at least 1 s more), `solved:` (an `unknown-ok` job that was solved) and
`replayed:` (a job with `replay: impossible` whose `sat` replayed: the
field is stale).
Logs go to a results directory, and the record of the run (date, tier,
configurations, commits, sea-vc checksum, verdict counts, failures) is
appended to `$SEA_BENCH_HOME/gates/<date>-<commit>.yaml` (none with
`--no-record`).

Before it starts a job, the gate waits until enough memory is free and
the load is low enough (`--min-free-gb`, `--max-load`). It runs 4 jobs in
parallel by default.

| recipe | command |
|---|---|
| `just bench-list` | `bench.py suites` |
| `just bench-smoke` | `bench.py gate smoke` |
| `just bench-canary` | `bench.py gate quick --tag canary`: a few jobs per family, all with counter bounds |
| `just bench-gate quick` | `bench.py gate quick --config default` |

## Establishing expectations

`bench.py establish <suite|family>...` runs the default configuration and
writes `expected`, `tier`, `time` and `established` into the manifests.
`sat` and `unsat` become the expected verdict; a timeout, unknown,
memory-out or error becomes `unknown-ok` with tier `hard`. It prints
`HARNESS-DISAGREE:` when a verdict differs from `harness-expected`. An
expected verdict is an observation: review the diff in the benchmark
repository before committing it, and explain any verdict that disagrees
with the harness.

`--record-counters KEY,...` also records the observed value of each key
as a seeded bound for every solved job without a deliberate bound for it.
A run that breaks a deliberate bound prints `COUNTER-OVER:`.
