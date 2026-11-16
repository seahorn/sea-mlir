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
  replay: impossible          # a sat of this job may not replay (below); the only value
  premise-class: B            # B | C: the --seam premises may fail on its counterexamples (below)
  sha256: ...
```

`replay: impossible` is for a sat job whose model the interpreter cannot
run: the program updates memory without `inplace`
([In place](design/Memory.md#in-place-the-inplace-attribute)), so sea-vc
prints `result: sat`, then error `vc.replay-impossible` and exits 1. The VC
is exact and the verdict is sat, but the counterexample is not confirmed.
bench.py records such a run as verdict `sat` with `replay=impossible` (the
`replay` column of `stats.csv`); the gate fails on it (`FAIL:
replay-impossible ...`) unless the job carries the field, and reports
`replayed:` (no failure) when a job with the field replayed after all.

### Premise classes

`--seam` assumes the pointer premises A-SEP, A-NULL and A-NOWRAP
([SeaM](design/SeaM.md)); its promise is conditional: if the program
satisfies them and the VC is sat, the program has a counterexample. A
program whose counterexamples may leave their object (a mutant that drops a
precondition, an out-of-range index) gives the solver a choice: one model
keeps the premises and replays, another runs into a different object's
cells, where the VC (which assumed A-SEP) and the interpreter disagree.
Which model the solver returns changes with its version, seed or the
encoding, so an exact verdict with a clean replay is the wrong expectation
for such a job. `premise-class` states what the job may do:

| class | field | the premises on the job's counterexamples | the gate accepts |
|---|---|---|---|
| A | absent | hold on every run | the expected verdict, clean replay (as for every job without the field) |
| B | `premise-class: B` | some counterexample keeps them | sat: confirmed, or a replay failure a premise violation explains; unsat fails |
| C | `premise-class: C` | every counterexample violates them | unsat, or a sat with a premise signal (below); a clean confirmed sat fails |

A replay failure a premise violation explains is one of

- sea-vc rc 2 with `replay: assert-failed` (the interpreter, which does not
  use the premises, reached the failing assertion on the model's inputs) and
  a `seam.inb-violation:` line (the A-INB check of the model's run: found,
  or `undetermined (...)` when the run reads a memory cell that check is not
  given), with the error `vc.replay-mismatch` (not `vc.replay-no-failure`
  of `--ptr-companion=check`): `premise-unconfirmed: <job> ... log=<path>
  rerun: <command>`;
- rc 4 (`vc.seam-inb-spurious`: the run accesses memory outside an object):
  `premise-inb-violation: ...`.

A run whose replay prints `replay: no-clobber-violated:` (a load of an
earlier memory version whose cell was overwritten: the program's Mem-SSA
claim, [Memory](design/Memory.md)) is never explained by a premise: it
fails on a declared job, and an undeclared job gets no hint for it.

A sat whose replay was clean but whose run of the original program left its
object (`replay.original: status=memory-error`, rc 0) counts as
`left-object` (outside an object is A-INB, stronger than A-SEP: such a
run may keep A-SEP). These three (left-object, unconfirmed,
inb-violation) are the premise signals. Any other replay failure of a declared job (rc 2
without those lines, rc 6, `original-memory-error`) fails as for every job.
The gate prints `premises: declared-runs=N left-object=N unconfirmed=N
inb-violation=N unsat-c=N hints=N` and records the counts; `rows.jsonl`
carries `replay_original_status`, `replay_status`, `seam_inb`,
`replay_no_clobber` and `replay_mismatch_diag` for every run.

On a class C job a sat needs a premise signal: a clean confirmed sat
refutes "every counterexample violates the premises" and fails as
`misclassified ... declare B`. `DISAGREE-SAT-UNSAT` between configurations
is waived for a class C job only when every sat run of it has a signal.

A job without the field is judged as before. When one of its runs shows a
signal above (rc 2 with both lines, rc 4, or rc 0 with
`replay.original: status=memory-error`), its `FAIL:` line ends with
`premise may fail on this job: declare it`, or, for a passing run, a
`premise-hint:` line says so. `establish` prints the same `premise-hint:`
and never writes the field. It records a declared job's explained sat
(rc 2 / 4 as above) as sat; for an unsat of a class B job it prints
`PREMISE-B-UNSAT:` and leaves `expected` as it was, and for a clean sat of
a class C job it prints `PREMISE-C-MISCLASSIFIED:`.

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
  (the gate runs with `--brunch-stats`: `vc.ackermann-*`, `seam.premises-*`, `assume.*`,
  `seam.rule-calls`, ...) or a `key: value` line of its stdout (the
  `seam.rule.*` counts need `--seam-stats` in the job's `args`). Values are
  numbers; a bound is `max`, `min` and / or `exact`.
- A key `<line>.<field>` names one entry of a `<line>: k=v k=v ...` line:
  `vc.box-all.expanded`, `vc.box-all.lowered`,
  `vc.expand-memmove.kept-over-k`. These record the lowering's form (a box
  expanded or lowered to a nondet index, a memmove expanded or kept), so a
  bound such as `{exact: 1}` makes a silent switch of form visible. A job
  whose lowering did not run prints no such line: the counter is missing.
  An exact key (BRUNCH_STAT or `key: value` line) takes precedence, and
  the first matching line counts.
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
own `args` are appended, then `--replay-original` when the sea-vc has the
option (the gate prints `replay-original: on`; `--no-replay-original` turns
it off): each `sat` is also replayed on the program before the lowering
([Replay on the original program](reference/output-formats.md#replay-on-the-original-program)).
Per-job timeout: the configuration's, capped by
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
  a timeout / unknown / error is `unsolved`); a class C job passes unsat
  and a sat with a premise signal and fails a clean sat (`misclassified`),
  a class B job fails unsat ([premise classes](#premise-classes));
- `replay-mismatch` (sea-vc rc 2: the model does not replay), unless a
  premise violation explains it on a job with `premise-class`;
- `replay-impossible` (a sat whose model the interpreter cannot run) on a
  job without `replay: impossible`;
- `lowering-mismatch` (sea-vc rc 6: the program before the lowering does
  not fail the assertion of the lowered program's `sat`);
- `DISAGREE-SAT-UNSAT`: two configurations solve a job differently.
- a counter outside its bound (configuration `default`; `FAIL: counter`,
  counted as `counter-failures`; see counter expectations above).

It reports, without failing, `slow:` (wall > `--slow` (2) x the manifest
time and at least 1 s more), `solved:` (an `unknown-ok` job solved),
`replayed:` (a job with `replay: impossible` whose sat replayed: the field
is stale), `premise-unconfirmed:` / `premise-inb-violation:` (a declared
job's explained sat, with its log and the command that reruns it) and
`premise-hint:` (a premise signal on a job without `premise-class`).

Logs and `rows.jsonl` go to `$SEA_BENCH_HOME/results/<stamp>-gate-<tier>/`.
With `--no-record`, no gate record is written; logs and results stay in
`$SEA_BENCH_HOME/results` as for any gate (`utils/attest.py` reads them).
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

A job whose counterexamples may leave their object declares it with
`premise-class` ([premise classes](#premise-classes)): mutants that drop a
precondition (a range check of an index, a length check),
mutants that shift or corrupt an index or offset, and harnesses that pass
out-of-range indices. Class B when a counterexample inside the objects
exists (the bug shows without leaving an object), class C when every
counterexample needs an access outside one; a class C job shows its bug
only conditionally, so prefer a job that shows it in bounds (for example
with the bounds check as the assertion). Undecided: B, and record why in
the job's `note`. `establish` prints `premise-hint:` when a run of an
undeclared job shows a premise signal; the field is curation, establish does
not write it.
