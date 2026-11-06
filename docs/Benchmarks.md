# Benchmarks

Unit tests (lit, `test/`) stay in this repository and ship with it. The
benchmarks do not: they are large, carry third-party code under its own
licenses, and sea-mlir users do not need them. They live in their own git
repositories and are connected to a checkout on demand:

| suite | contents | access |
|---|---|---|
| `public` ([seahorn/sea-mlir-bench](https://github.com/seahorn/sea-mlir-bench)) | `vcc` (aws-c-common via verify-c-common), `firedancer`, `stellar`, `nki`, `crafted` | Apache-2.0 with attribution |

No git submodules: `just` recipes connect benchmark checkouts into the
working directory. The whole suite is too expensive for CI; gates are run by
hand (smoke is the CI candidate) and their records are committed here.

## Connect

```sh
just bench-connect        # python3 utils/bench.py connect
```

reads `bench/sources.yaml` (committed: the source names and the public URL),
overlaid by `bench/sources.local.yaml` (gitignored, per machine) and
`SEA_BENCH_SOURCE_<NAME>=<url-or-path>`:

```yaml
sources:
  public: https://github.com/seahorn/sea-mlir-bench.git   # URL -> clone, later pull --ff-only
```

Each source becomes `bench/work/<name>` (gitignored). A source without a
value is skipped. `SEA_BENCH_SUITES=<dir>:<dir>` bypasses `bench/work/`
altogether.

## Layout of a benchmark repository

    <family>/<job>.mlir        a verification task (sea.program)
    <family>/manifest.yaml     defaults (args, timeout, mem), established, jobs
    <family>/PROVENANCE.md     sources, export commands, license decision
    README.md, LICENSE*, NOTICE*

A manifest job:

```yaml
frag-reach-busy-K1:
  expected: unknown-ok        # sat | unsat | unknown-ok
  tier: hard                  # smoke <5 s | quick <60 s | full <900 s | hard (tracked, not gated)
  time: 189.48                # wall seconds when established
  established: unknown        # the verdict observed then
  harness-expected: sat       # the harness author's expectation, if stated
  tags: [reach]               # proof | reach | mutant | blowup-regression | inb-regression | unsafe
  args: [--ptr-mode=address]  # appended to the configuration's args
  mutant: mut-bounds-1        # for tag mutant
  export: {...}               # how the .mlir was made (repo, commit, command, bound, patches)
  counters: {...}             # completeness bounds, below
  sha256: ...
```

### Counter expectations (completeness)

A verdict says nothing about how the answer was reached: a change that makes
SeaM miss an equal address can preserve every verdict while giving the solver
more work. Counter bounds make that visible:

```yaml
defaults:
  counters: {vc.ackermann-constraints: {max: 100}}   # every job of the family
counter-rules:                                        # jobs whose name matches `jobs` (a regex)
- jobs: ^sweep-gather-.*-(good|nopre)$
  counters:
    vc.ackermann-constraints: {max: 0}
  why: "design promise: SeaM resolves every read of gather"
jobs:
  sweep-scatter-N2-D2-K1-pmax2-fmax1-good:
    counters:
      vc.ackermann-constraints: {max: 52, seeded: <date> sea-vc <revision>}
      vc.ackermann-reads: {max: 28, seeded: ...}
```

- A key is any statistic sea-vc prints: a `BRUNCH_STAT <key> <value>` line
  (the gate runs with `--brunch-stats`: `vc.ackermann-*`, `seam.inb-*`,
  `seam.rule-calls`, ...) or a `key: value` line of its stdout (the
  `seam.rule.*` counts need `--seam-stats` in the job's `args`). Values are
  numbers; a bound is `max`, `min` and / or `exact`.
- The effective bounds of a job: `defaults.counters`, then each matching
  `counter-rules` entry in order, then the job's own `counters`; a key set to
  `null` removes an inherited bound.
- A bound with `seeded` was recorded by `establish --record-counters` (an
  observation, an upper bound by default). A bound without it is deliberate:
  it encodes a design promise, carries a `why`, and establish never changes
  it.
- `gate` checks the bounds of every job that answered as expected under
  configuration `default` (other configurations change the counts). A
  violation fails the gate and is reported apart from verdict failures:
  `FAIL: counter <job> config=default <key>=<value> bound max=<n> why: ...`,
  `counter-bounds:` (checked) and `counter-failures:` lines, and
  `counter-failures` in the record. A bounded counter missing from the output
  is a violation (`=missing`): a renamed statistic must not pass silently.
  `--no-counters` skips the check.

Seeded bounds are observations recorded by the benchmark establishment tool.
Deliberate bounds encode a design promise and include a `why` field.

### The canary gate

`just bench-canary` (`bench.py gate quick --tag canary`) runs the jobs tagged
`canary`: 2-4 per family (vcc, firedancer, stellar, crafted, nki: gather
sweep, gather hardware size N64, dynamic add, scatter-add), all with counter
bounds, about 15 s at `--par 4`. `--tag T` selects the jobs with tag `T` in
any gate.

`defaults` is the configuration the verdicts were established with: today
`--lower --seam --widen-memory=4` for the C applications, `=8` for vcc,
`--lower --seam` for NKI; timeout 900 s, memory 16 GB (RLIMIT_AS). A job
that needs more is `hard` / `unknown-ok`; the cap is not raised for it.

## Commands

```sh
just bench-list                      # bench.py suites: families, jobs per tier / verdict
just bench-smoke                     # bench.py gate smoke
just bench-gate quick                # smoke + quick jobs
just bench-gate full default,default+yices   # two configurations: also checks sat/unsat agreement
python3 utils/bench.py gate quick --suite public/firedancer --jobs 'hpack-' -v
python3 utils/bench.py establish public/stellar             # (re)establish expected verdicts
```

`bench.py --agent` has the terse reference. sea-vc is
`build-release/bin/sea-vc` (else `build/bin/sea-vc`, `$SEA_VC`, `--sea-vc`).

### gate

`bench.py gate <tier>` runs every job of that tier and the lower ones (hard
only with `gate hard`), with each configuration of `--config` (comma
separated): `default` (the manifest's defaults), `default+CFG` (defaults
plus `bench/configs/CFG.yaml`'s args and solver), or `CFG` alone. A job's
own `args` are appended. Per-job timeout: the configuration's, capped by
the job's tier (smoke 60 s, quick 300 s). At most `--par` (default 4) jobs
in parallel.

Gate and establish start jobs only when available memory and load average
meet `--min-free-gb` and `--max-load`; otherwise they wait (`wait:` line).
The free-memory threshold defaults to the smaller of 60 GB and one quarter
of total memory; the maximum load average defaults to 32. Adjust these for the
host, and use `--par` and `--mem` to control concurrency and per-job memory
(default manifest cap: 16 GB).

The gate fails (exit 1) on

- a verdict other than `expected` for a sat / unsat job (a flip is `WRONG`,
  a timeout / unknown / error is `unsolved`);
- `replay-mismatch` (sea-vc rc 2: the model does not replay);
- `DISAGREE-SAT-UNSAT`: two configurations solve a job differently.
- a counter outside its bound (configuration `default`; `FAIL: counter`,
  counted as `counter-failures`; see counter expectations above).

It reports, without failing, `slow:` (wall > `--slow` (2) x the manifest
time and at least 1 s more) and `solved:` (an `unknown-ok` job solved).

Logs and `rows.jsonl` go to `$SEA_BENCH_HOME/results/<stamp>-gate-<tier>/`.
The gate record (date, tier, configs, sea-mlir commit and dirty flag, sea-vc
sha256, suite revisions, verdict counts, failures, slow jobs) is appended to
`$SEA_BENCH_HOME/gates/<date>-<commit>.yaml` alongside benchmark results.

### establish

`bench.py establish <suite|family>...` runs the default configuration and
writes `expected`, `tier`, `time` and `established` into the manifests:
sat / unsat become the expected verdict, a timeout, unknown, memout or error
becomes `unknown-ok` with tier `hard`. `--resume <results dir>` reuses the
finished runs of an interrupted establish. With `--no-run` it only writes the manifests from
those runs; jobs without a run get `expected: pending` (no tier: not gated). It prints `HARNESS-DISAGREE:` when a
verdict differs from the harness author's `harness-expected`. Review the
diff in the benchmark repository before committing it: an expected verdict
is an observation, and one that disagrees with the harness or the
application notes needs an explanation, not a silent record.

`--record-counters KEY,...` also writes the observed value of each KEY as a
seeded bound `{max: <value>, seeded: <date> sea-vc <sha>}` for every solved
job without a deliberate bound for it (an older seeded bound is replaced); a
run that breaks a deliberate bound prints `COUNTER-OVER:`. With
`--counters-only` it writes only that (expected / tier / time / established
stay as they are).

## Adding benchmarks

Add `<family>/<job>.mlir` with a manifest entry (`expected: null`,
provenance under `export`), run `establish` for that family (`--jobs` to
limit it), check the verdicts against the harness, commit in the benchmark
repository. A new family goes to the public set only after its license is
checked. Mutants (expected sat) are jobs tagged `mutant`.
