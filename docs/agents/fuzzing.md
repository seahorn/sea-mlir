# Steering the fuzzer

`sea-run --fuzz` runs a program on generated inputs and reports what the
runs reached ([Fuzzing](../reference/fuzzing.md) is the reference). This
page is the loop an agent runs on a real program: run a bounded campaign,
read the output, change one flag or input, run again. Every step below
names the line to read and what it costs.

A campaign without failures says nothing about the inputs it did not try.
`sea-vc` checks all of them.

The defaults of the fuzzer (the dictionary share, the range share, the
keep rule, the MCMC `beta`, weight and restart bound, the sync period)
are not tuned. They were measured on small programs only. Treat each as a
knob to try, not as a recommendation, and report the values a result
came from.

## The loop

**0. Look before running.** `--fuzz-sites` lists the input sites, every
assume with its condition over the sites, and the ranges the fuzzer reads
off the assumes. It runs nothing.

```console
$ sea-run p.mlir --fuzz-sites
```

**1. Run a bounded campaign** with progress lines and a fixed seed:

```console
$ sea-run p.mlir --fuzz=200000 --fuzz-time=60 --seed=1 \
    --fuzz-progress=5 --fuzz-out=out1 --fuzz-gates
```

`--fuzz-out` must name a new or empty directory; use a new one per run.

**2. Read** the `fuzz.progress` lines while it runs and the `fuzz.*`
summary at the end ([Reading the output](#reading-the-output)).

**3. Decide** one change from the [symptom table](#symptoms-and-actions).

**4. Rerun** with that change, a new `--fuzz-out`, and the corpus of the
earlier run as seeds (`--corpus=out1`), so the new run starts where the
old one stopped. A `failure-*.txt` file among the seeds fails again at
once; pass the `cov-*.txt` files only when the failure is known. Change one thing at a time; with the same seed and
options a run is reproducible ([Reproducibility](../reference/fuzzing.md#reproducibility)),
so two runs that differ in one flag can be compared.

Stop when the [stop conditions](#when-to-stop) hold.

## Cost and budget

| unit | what it measures | how to bound it |
|---|---|---|
| trials | runs of the program | `--fuzz=<n>` |
| wall time | seconds of the campaign | `--fuzz-time=<s>` (with `--fuzz`: whichever ends first) |
| steps | operations executed, over all trials: the work done | read `steps=` and `steps/s=` of `fuzz.trials`; no flag bounds the total |
| steps per trial | one run | `--max-steps=<n>` (default 1000000); a trial past it ends with status `step-limit` |

A trial an early assume blocks costs a few steps; one that reaches the
end costs all of them. Compare campaigns by steps, not trials. A run with
`--fuzz-time` is reproducible only up to where the time ran out; give
`--fuzz` alone for a fixed run.

One process uses one core. `utils/fuzz-par.py -n <k>` uses `k` cores for
the wall time given; on a shared machine, keep `k` within the machine's
limit for heavy jobs. `--fuzz-sync` (and `fuzz-par.py --sync`) adds
directory reads every period and makes the run not reproducible.

`fuzz.status` shows where the steps went. Many `step-limit` trials mean
loops that run long on random inputs: each costs `--max-steps` steps. A
lower `--max-steps` makes them cheaper and the trials that need long runs
impossible; the step-limit trials are not failures and not coverage past
the point they stopped.

Exit status 0 means the trials ran: an assertion failure is in the output
(`fuzz.failure`), not in the exit status. 4 is a memory-safety finding
under `--defer-memory-errors`; 1 is a bad option or corpus, or a failure
that does not replay (`fuzz.replay-mismatch`, a bug in sea-mlir).

## Reading the output

The examples are runs of the programs in `test/Interp/fuzz/`.

### `fuzz.progress`: is the run still finding anything

```text
fuzz.progress: 0.010 trials=1579 steps=12632 blocks=1 edges=1 paths=1 assumes=0 failures=0 frontier=0 cmp-buckets=2 corpus=1 cost=18
fuzz.progress: 0.020 trials=3216 steps=25728 blocks=1 edges=1 paths=1 assumes=0 failures=0 frontier=0 cmp-buckets=2 corpus=1 cost=13
fuzz.progress: 0.030 trials=4874 steps=38992 blocks=1 edges=1 paths=1 assumes=0 failures=0 frontier=0 cmp-buckets=2 corpus=1 cost=7
```

- `blocks`, `edges`, `assumes`, `frontier` growing: the run reaches new
  behavior; let it go on.
- `cmp-buckets` growing while blocks are flat: the runs come closer to
  comparisons they have not flipped; more time can still pay.
- Everything flat for several periods: more of the same run is unlikely
  to help. Change something.
- `cost=` (with `--fuzz-strategy=mcmc` only) falling: the chain closes in
  on the gate that blocks it. Above, the cost went 18, 13, 7 and the next
  trial failed the assertion. A cost that does not fall for many periods
  is a stall ([MCMC](#fuzzmcmc-the-chain)).
- `steps` divided by `trials`: the mean cost of a trial.

### `fuzz.frontier` and `fuzz.gate-closed`: where the runs stop

```text
fuzz.frontier: deepest=0 trial=1 end=assume-blocked gate=0 mean=0.00 assumes=1 passed=0 closed=1 unreached=0
fuzz.gate-closed: 0 blocked=20000 distance=31578 check=1 at=mcmc.mlir:90:3 sites=0 cond=(eq (add (mul s0 3) 7) 1000003)
```

The frontier of a trial is the number of distinct assumes it passed.
`gate=` names the assume that blocked the deepest trial. A closed gate
is reached by trials and passed by none: it is the first lead. Its
`cond=` is the condition over the input sites; read it to choose inputs.
Here site 0 must satisfy `3 * s0 + 7 = 1000003`, that is `s0 = 333332`.

- `distance=` is the least branch distance over the run: how far an
  operand had to move. A large distance that shrinks between runs is
  progress; one that stays put is not.
- `distance=1` on an `ne` condition means nothing: `ne` gives 1 on every
  blocked trial.
- `distance=-`: the condition is not a comparison (an `and`, an `or`, or
  `?` in `cond`); the fuzzer has no measure of how close the runs came.
- `check=1`: the assume is followed by `assert false`. In SeaHorn's export
  form passing it is a failure, and a proof keeps it closed
  ([SeaHorn's export form](../reference/fuzzing.md#seahorns-export-form)).
  A closed `check=1` gate is what a correct program looks like.
- `unreached=` counts assumes no trial got to: they are behind the closed
  gates.

`fuzz.blocked` lists every blocking op, most first, with the same
`cond=`. A harness whose runs are almost all blocked by one assume tests
little beyond it.

### `fuzz.cmp`: how close the comparisons came

```text
fuzz.cmp: 2 reached=2 buckets=12/14 outcomes=4/4 assume=6/7 branch=6/7 value=0/0
```

Each comparison has seven buckets of `lhs - rhs`: far below, near below,
-1, 0, +1, near above, far above. `outcomes` below the total means some
comparison went only one way. `assume=` low and the rest high: the
assumes are the obstacle. The per-comparison counts are in the
`cmp:` lines of `--fuzz-coverage-out`.

### `fuzz.corpus`: what the corpus kept

```text
fuzz.corpus: 7 seeds=0 skipped-lines=0 files=0 dictionary=3 keep=blocks,cmp kept-blocks=2 kept-cmp=5 kept-frontier=0 dropped=0
```

- `kept-<reason>`: the trials kept for each reason of `--fuzz-keep`
  (first reason counts). `kept-cmp` is what `cmp` added beyond `blocks`.
- `dropped=`: trials kept past `--fuzz-corpus-max`. Many drops: the bound
  is too small for the program.
- `seeds=`, `skipped-lines=`: the `--corpus` files read, and lines in them
  that are not inputs (the rest of a saved run).
- `dictionary=`: the constants of the program and their neighbors.

A corpus that stays at 1 or 2 entries while the run is long: nothing new
is found, so the mutations always start from the same input.

### `fuzz.sync`: what the peers gave

```text
fuzz.sync: imported=1 kept=1 skipped=1 polls=5 dirs=2 missing=1 period=0.1
```

`imported` files of the peers ran as trials, `kept` of them joined this
corpus. `kept=0` over a long run: the peers find nothing this process
has not; sync costs reads and nothing else. `skipped` counts files that
do not parse or do not fit (a warning `fuzz.sync-skip` names each kind);
`missing` counts directories that did not exist at the last poll (a peer
not started, or a wrong path).

### `fuzz.mcmc`: the chain

With `--fuzz-strategy=mcmc` ([MCMC strategy](../reference/fuzzing.md#mcmc-strategy)):

```text
fuzz.mcmc: moves=5362 accepted=2193 uphill=95 restarts=2 restarts-rejected=0 failure-restarts=1 best-cost=0 best-trial=5365 stall=0:5255 beta=1 restart=1000 share=1
fuzz.gate: 0 op=sea.assume passed=1 blocked=5364 distance=3 measured=5364 mcmc-moves=5362 mcmc-improved=107 check=1 at=mcmc.mlir:90:3
```

| field | read it as |
|---|---|
| `accepted / moves` near 1, `uphill` large | `beta` too small: the chain walks at random |
| `accepted / moves` near 1, `uphill=0` | every move has the same cost: no gradient (see `stall`) |
| `accepted / moves` low, `uphill` near 0 | the chain sits in a local minimum; only restarts move it |
| `restarts` | restarts after `--fuzz-mcmc-restart` moves without a new best |
| `restarts-rejected` | restarts whose input erred or hit the step limit; many: the corpus entries are bad starting points |
| `failure-restarts` | failing trials of the chain |
| `best-cost` | the least cost of a chain trial, against the deepest frontier; 0: the chain reached the deepest frontier with nothing left to cross |
| `stall=<id>:<n>` | the gate where most moves did not lower the cost |
| `mcmc-improved` on `fuzz.gate` | moves from a trial blocked at that gate that lowered the cost; 0 with many `mcmc-moves`: the cost does not lead past this gate |

The cost is `65 * (deepest - frontier) + bitlen(distance)`: within one
gate it is the bit length of the distance (`best-cost=8` for a least
distance of 183).

## Symptoms and actions

| symptom (lines) | action |
|---|---|
| `fuzz.reachable: covered=N reachable=N` | every block `sea-vc` calls reachable is covered: block coverage is done; stop or move to a property question |
| `fuzz.uncovered` lines and a flat `fuzz.progress` | the blocks named are reachable and missed: inspect the gate before them (`fuzz.gate-closed`, `--fuzz-gates`), seed past it |
| one `fuzz.gate-closed` with `cond=` over sites, constant compared directly (`(eq s0 1234)`) | the dictionary holds the constant; check it is on (`--fuzz-dict` not false), raise `--fuzz-dict-share` |
| a closed gate whose `cond` you can solve by hand or with z3 | write the input as a corpus file (`0=333332`) and pass `--corpus=`; the run starts past the gate |
| a closed gate with a comparison condition and a large `distance=` that does not shrink | try `--fuzz-strategy=mcmc` (the branch distance leads the chain) |
| closed gate with `distance=1` on `ne`, or `distance=-` | no gradient: the cost cannot lead past it; MCMC will stall there (`stall=<id>:<moves>`, `mcmc-improved=0`). Read `cond`, seed past it, or ask `sea-vc` |
| MCMC `accepted` close to `moves` and `uphill` large | raise `--fuzz-mcmc-beta` |
| MCMC `accepted` low and `uphill` near 0 for a long time, no failure | lower `--fuzz-mcmc-beta`, or lower `--fuzz-mcmc-restart` to restart sooner |
| MCMC stalls but the corpus strategy found coverage before | mix them: `--fuzz-mcmc-share=0.5` |
| a range-bounded site whose ends matter (`fuzz.range` lines) | raise `--fuzz-range-share` toward 1; bugs needing values outside the range need it lower |
| corpus at 1-2 entries, comparisons covered but blocks flat | `--fuzz-keep=blocks,cmp` keeps inputs that came closer to a comparison |
| deeper frontier matters more than blocks | `--fuzz-keep=blocks,frontier` |
| `--fuzz-keep=cmp` or `all` makes a large corpus (`kept-cmp=` high) and the picks rarely reach the entries that matter | bound it with `--fuzz-corpus-max` and read `dropped=` |
| many `step-limit` in `fuzz.status` | long loops on random inputs: lower `--max-steps`, or seed bounded loop counts |
| `fuzz.memory-check` lines | a run stopped at a memory-safety check; feasibility unknown: rerun with `--defer-memory-errors` to classify |
| `fuzz.limit` lines | the interpreter cannot run that op: not a finding; lower the program first ([Programs that do not run](../reference/fuzzing.md#programs-that-do-not-run)) |
| a long campaign is needed and there are cores | `utils/fuzz-par.py -n <k> --out <dir> --sync -- p.mlir <args>`; read `fuzz-par.model` for the joined frontier and closed gates |

`utils/fuzz-par.py --agent` lists its lines. Its `fuzz-par.model` line
joins the gates of all processes:

```text
fuzz-par.model: frontier-max=0 closed=1 closed-gates=0 cmp-buckets=3/14 closed-distance=0:1
```

## Example: a gate with no gradient

A check in SeaHorn's export form is an assume of its failure, then
`assert false`. When the check is an equality `r == x`, the assume is
`r != x`, and every trial it blocks has `r = x`: distance 1 always. In
this program `r` differs from `x` only when `3 * x + 7 = 1000003`:

```text
fuzz.mcmc: moves=199800 accepted=199800 uphill=0 restarts=199 restarts-rejected=0 failure-restarts=0 best-cost=1 best-trial=1 stall=0:199800 beta=1 restart=1000 share=1
fuzz.gate-closed: 0 blocked=200000 distance=1 check=1 at=p.mlir:14:3 sites=0 cond=(ne (add s0 (select (eq (add (mul s0 3) 7) 1000003) 1 0)) s0)
fuzz.gate: 0 op=sea.assume passed=0 blocked=200000 distance=1 measured=200000 mcmc-moves=199800 mcmc-improved=0 check=1 at=p.mlir:14:3
```

Every move is accepted, none uphill, none improved: the chain drifts. The
corpus strategy on the same trials does not pass the gate either: the
dictionary holds 1000003 and its neighbors, not the solution. Changing
`beta` does not help; there is no cost difference to act on. Two ways
past:

- read `cond`: the inner comparison `(eq (add (mul s0 3) 7) 1000003)`
  gives `s0 = 333332`; a corpus file `0=333332` fails at trial 1;
- `sea-vc p.mlir`: `result: sat` with `nondet: ... value=333332`, replayed.

This is a limit of the cost, not a bug: how close a run came to
violating an equality is not in the operands of the check
([Model coverage](../reference/fuzzing.md#model-coverage)).

## Example: the same gate with a gradient

`@narrow` in `test/Interp/fuzz/mcmc.mlir` assumes the equality itself,
`(eq (add (mul s0 3) 7) 1000003)`, then asserts false. With 20000 trials
and seed 1 the corpus strategy keeps it closed (`distance=31578`); MCMC
fails it at trial 5365 (the `fuzz.mcmc` line above). With 3000 trials the
chain does not get there at `beta=1` (`best-cost=8`), and at `beta=0` it
walks at random (`accepted=2997` of `2997`, `uphill=722`,
`best-cost=15`). One program and one seed are not a tuning result; they
show what the fields look like when the chain works and when it does not.

## When to stop

- `fuzz.failure` (or `fuzz.memory-safety`): replay its file with
  `sea-run p.mlir --input-file=<file>` and report it. Further fuzzing
  finds the same failure; `--keep-going` counts the others.
- `fuzz.reachable` covers every reachable block, and the remaining closed
  gates are `check=1` gates: the campaign has nothing left to reach by
  blocks. The checks are proved or refuted by `sea-vc`, not by more
  trials.
- Flat progress after the changes of the table: ask the solver.

## Switching to the solver

`sea-vc` decides what fuzzing cannot: a closed gate that no input passes,
and the checks of a proof. `sea-vc p.mlir --reach=blocks` decides which
blocks are reachable (one query per block; `--reach-rlimit` bounds each);
pass its output to `--fuzz-reachable` to know the ceiling of block
coverage. The fuzz corpus helps the solver: `sea-vc --reach
--reach-corpus=out1` runs the corpus files first and queries only the
targets they did not reach ([Command line](../reference/cli.md)).

`sea-vc --z3-tactic=sls-smt --timeout=<s>` runs z3's local search on the
query. It answers `sat` or `unknown`, never `unsat`; always give a
timeout. On the small mutants measured so far it found no bug in 120
seconds, where the default z3 solve found them in seconds
([Local search in the solver](../reference/fuzzing.md#local-search-in-the-solver)).
Try it as a cross-check, or when the default solve times out on a query
that looks satisfiable.

## What to report

For each campaign: the output of `sea-run --version`, the full command (seed, strategy and every `--fuzz-*` value), the
corpus it started from, `fuzz.trials` (trials, steps, time), `fuzz.status`,
`fuzz.coverage`, `fuzz.frontier` with the closed gates, `fuzz.mcmc` for
MCMC runs, and any failure with its file. Say which defaults were left
as they are.
