# Fuzzing

`sea-run --fuzz` runs a program many times on generated nondet values and
reports what the runs reached. Each trial is a `sea-run` run of the
program: it has the interpreter's semantics, the semantics `sea-vc`
encodes. A failing trial is a run that fails an assertion, or, with
`--defer-memory-errors`, a run with a memory-safety violation that no
assumption blocks.

A fuzzing run without failures says nothing about the inputs it did not
try; `sea-vc` checks all of them.

## Running it

```console
$ sea-run p.mlir --fuzz=10000 --seed=1 --fuzz-out=out
```

| option | effect |
|---|---|
| `--fuzz=<n>` | run `n` trials |
| `--fuzz-time=<s>` | run for at most `s` seconds (with `--fuzz`: whichever ends first) |
| `--seed=<n>` | the seed of every random choice |
| `--corpus=<dir or file>,...` | seed entries: corpus files, or directories of them (their files in name order) |
| `--fuzz-out=<dir>` | write failing inputs and inputs that reached new coverage as corpus files; the directory must be new or empty |
| `--fuzz-dict=false` | no dictionary values (see below) |
| `--fuzz-dict-share=<p>` | the probability that a fresh value and that a mutation is a dictionary value; unset: 1/2 of fresh values and 1/5 of mutations (see below) |
| `--fuzz-ranges=false` | no range draws: the generator without them (see [Ranges](#ranges)); default on |
| `--fuzz-range-share=<p>` | the probability that a trial uses range draws; the other trials use the generator without them (default 0.5; see [Ranges](#ranges)) |
| `--fuzz-progress=<s>` | print a `fuzz.progress` line every `s` seconds (see [Progress](#progress)); default 0: none |
| `--fuzz-coverage-out=<file>` | write the covered blocks, edges, assumes and paths by id at the end (see [Progress](#progress)) |
| `--fuzz-cmp` | comparison coverage: operand distance buckets of every comparison, branch distances of the blocking assumes (see [Model coverage](#model-coverage)); default on, `--fuzz-cmp=false` turns it off |
| `--fuzz-gates` | one `fuzz.gate` line per `sea.assume` and `sea.unreachable`: trials that passed it and that it blocked (see [Model coverage](#model-coverage)) |
| `--fuzz-reachable=<file>` | the output of `sea-vc --reach=blocks` on the same file: block coverage of the reachable blocks, the uncovered ones named (see [Model coverage](#model-coverage)) |
| `--fuzz-keep=<reason>,...` | why a trial joins the corpus: `blocks` (default), `cmp`, `frontier`, `all` (see [Corpus feedback](#corpus-feedback)) |
| `--fuzz-corpus-max=<n>` | at most `n` corpus entries; kept trials past it are dropped and counted (default 0: no bound) |
| `--fuzz-sync=<dir>,...` | every `--fuzz-sync-period` seconds, run the new-coverage files of other processes' `--fuzz-out` directories as trials (see [Corpus sync](#corpus-sync)) |
| `--fuzz-sync-period=<s>` | the seconds between two reads of the `--fuzz-sync` directories (default 5, not tuned) |
| `--keep-going` | count failures instead of stopping at the first |
| `--defer-memory-errors` | go on past memory-safety violations; one is a failure only if the run reaches the end (see below) |
| `--input-file=<file>` | run once on a corpus file (without `--fuzz`) |
| `--max-steps=<n>` | the step limit of each trial |
| `--fuzz-sites` | list the input sites and exit, without running (see [For agents](#for-agents)) |
| `--fuzz-serve` | run on inputs sent as JSON lines on stdin (see [Driving the fuzzer from Python](#driving-the-fuzzer-from-python)) |

All trials run in one process: the program is parsed once.

## Trials

The seeds run first, in order. Every later trial picks an entry of the
corpus, or the empty entry, and changes one, two, four or eight of its
values. A change is one of:

- a fresh random value;
- one bit flipped;
- plus or minus one;
- 0, 1, -1, or the signed minimum or maximum of the width;
- a dictionary value.

A site or memory cell that the entry does not set gets a random value or a
dictionary value. Every value fits the width of its site. In a range
trial (below), a value site with a range gets values from its range
instead of random ones.

The dictionary holds the integer constants that the program compares with
or computes with (`sea.icmp`, add, sub, mul, div, rem, min, max and the
overflow tests), each with its neighbors plus and minus one. A value is
sign- or zero-extended or truncated to the site's width. It gets runs past
guards such as `assume(x == 1234)` that random values miss.

By default, half of the fresh values and one mutation in five are
dictionary values. `--fuzz-dict-share=<p>` sets both to `p`: a fresh value
is a dictionary value with probability `p`, and a mutation too, otherwise
one of the four other kinds, each equally likely. `--fuzz-dict=false`
keeps the default mix with a random value in place of each dictionary
value, so a mutation is then a fresh random value two times in five; it
does not go with `--fuzz-dict-share`. No share is best for every
program: one bug needs a constant of the program, another needs the 0, 1
and plus-or-minus-one changes, which a larger share makes rarer.

### Ranges

Many harnesses bound an input with an assume: `assume(n < 64)`, or
`assume(n - 1 < 64)` for n in 1..64. Random 64-bit values almost never
pass such an assume, and the runs that pass rarely reach the ends of the
range, where off-by-one bugs are. The fuzzer reads a range off each
`sea.assume` whose condition has one of these forms (seen through block
arguments with one incoming value), for a `sea.nondet` site `sN` of at most
64 bits:

| condition | range of `sN` |
|---|---|
| `(ult sN K)`, K > 0 | `[0, K-1]` |
| `(ult (add sN M) K)`, K > 0 | `[d, d+K-1]` modulo 2^w, d = -M modulo 2^w |
| `(ule sN E)` | `[0, E]`, E a sum (`add`) of value sites and constants, evaluated in each trial on the values read so far |

Several such assumes of one site intersect when their low ends agree; one
with another low end is not used, nor is `(ult sN 0)` (no value passes
it), nor a bound whose sites lead back to the site. A bound that names a
site the trial has not read yet is not used in that trial. Such unused
assumes are counted as `skipped` in `fuzz.ranges`. An assume need not
dominate the site's other uses: a range read off an assume on one path
also shapes the values on paths that do not pass it.

A site with a range gets its fresh values from it, and so does the
mutation "a fresh random value" (and, with `--fuzz-dict=false`, the
random value in place of a dictionary value). A draw from `[lo, hi]` is
`lo`, `hi`, `lo + r` or `hi - r` with r below 64 (or below the size of the
range), each with probability 1/6, or a uniform value in the range with
probability 2/6. The other mutations stay as they are: a bit flip, plus or
minus one, or 0, 1 or -1 may leave the range, and the assume then blocks
the run; such changes find bugs that need small or equal values.

Each trial is either a range trial, which draws as above, or a trial of
the generator without ranges. `--fuzz-range-share=<p>` is the probability
of a range trial (default 0.5). The choice is one draw of the seeded random
stream before the trial's input, so a seed gives the same trials. There is
no such draw when `p` is 0 or 1 or the program has no range: with 1 every
trial is a range trial, with 0 the trials are those of
`--fuzz-ranges=false`. Range draws keep a site in its range, which finds
the bugs at its ends sooner, but makes rarer the values that the generator
without ranges reaches by plain mutation, and some bugs need those. The
default 1/2 keeps both; it is not tuned.

The range only guides the choice of values. Every trial still runs the
assume. `--fuzz-ranges=false` turns the draws off: the trials are then the
same as those of the generator without ranges, for the same seed.
`--fuzz-sites` lists the ranges (`fuzz.range` lines,
[For agents](#for-agents)).

A site has one value per trial, as with `--input`: a site that a loop reads
again reads the same value. So every trial replays through `--input-file`.

## Coverage

A trial reaches new coverage when it enters a block it has not entered
before, takes a new edge between blocks, or passes a `sea.assume` that no
earlier run passed. By default its input then joins the corpus, and with
`--fuzz-out` it is written as `cov-<trial>.txt` ([Corpus
feedback](#corpus-feedback) has the other reasons).

The fuzzer also counts paths: a path is the sequence of blocks a trial
entered, whatever ended the trial, kept as a 64-bit hash. Paths are a
measure, not corpus feedback: a trial that reaches only a new path does
not join the corpus. Hashes can collide, so the count is a lower bound. A
loop-free program has finitely many paths; a program with loops has
unboundedly many (each iteration count is a path), and at most 2^20 are
counted (`paths-capped=` on `fuzz.coverage`).

## Progress

Trials are a poor measure of the work done: a trial that an early assume
blocks costs a few steps, one that reaches the end costs all of them. The
fuzzer counts interpreter steps (operations executed) over all trials as
its cost (`steps=` and `steps/s=` of `fuzz.trials`).

`--fuzz-progress=<s>` prints one line every `s` seconds of the run, after
the trial that crossed the time, and flushes it:

```text
fuzz.progress: 1.000 trials=183513 steps=1449724 blocks=4 edges=5 paths=3 assumes=1 failures=0 frontier=1 cmp-buckets=9 corpus=4
```

The fields are the elapsed seconds, then the totals so far: trials,
steps, blocks entered, edges taken, distinct paths, `sea.assume` ops
passed, failing trials, the deepest assume frontier, unless
`--fuzz-cmp=false` the comparison buckets covered (`cmp-buckets=`; see
[Model coverage](#model-coverage)), and the corpus entries. While blocks, edges, paths or assumes still grow,
the fuzzer is reaching new behavior; when they stay flat for a long time,
more of the same is unlikely to help, and a corpus entry, another
`--fuzz-range-share` or a look at the `fuzz.blocked` lines is the next step.

`--fuzz-coverage-out=<file>` writes what the run covered, by id, when it
ends:

```text
# sea-run --fuzz coverage: program=@main trials=514547
totals: blocks-total=4 assumes-total=1 paths-capped=0
block: 0
block: 1
block: 2
block: 3
edge: -1 0
edge: 0 1
edge: 0 2
edge: 1 3
edge: 2 3
assume: 0
path: 0x6d9ce9a95edf3676
path: 0x6da0eba95f4570e7
path: 0xaf63bc49fd1c5448
```

A block's id is its position in the program body (the entry is 0); an
edge is `<from> <to>`, `-1` for the entry of the run; an assume's id is
the one `--fuzz-sites` lists. The ids are the same in every process on
the same program file, so the files of several runs can be joined.

After the paths come the model coverage lines
([Model coverage](#model-coverage)): `frontier: deepest=<n> sum=<n>`
(the sum over the trials), one `gate: <id> op=<op> passed=<n>
blocked=<n>` per `sea.assume` and `sea.unreachable` (`distance=<d>|-`
with `--fuzz-cmp`), and with `--fuzz-cmp` one line per comparison:

```text
cmp: 0 pred=ult role=assume counts=0,3,0,1,0,1,0 at=p.mlir:4:9
```

`counts` are the evaluations per bucket, in the order `-L -S -1 0 +1 +S
+L`.

### Several processes

The fuzzer is single-threaded. `utils/fuzz-par.py` runs several `sea-run
--fuzz` processes with distinct seeds, each with its own `--fuzz-out`
directory and coverage file, and prints a merged progress line, the first
failure and the coverage of each process and of their union:

```console
$ python3 utils/fuzz-par.py -n 8 --out par --progress 5 -- p.mlir --fuzz-time=600
```

Run `utils/fuzz-par.py --agent` for its options and output lines. Without
`--sync` the processes are independent: the time to the first failure of
`n` processes is the least of `n` independent runs. With `--sync` each
process also reads the others' new-coverage files ([Corpus
sync](#corpus-sync)); `fuzz-par.process` lines then show what each
imported. Its `fuzz-par.model` line joins the model coverage of the files:
the deepest frontier, the assumes no process passed, and the buckets of all processes and the least distance of each closed
assume (the comparison fields are absent with `--fuzz-cmp=false`).

## Model coverage

Blocks and edges are what a code fuzzer counts. The program here is a
model: its assumes say which inputs count, and its comparisons decide
where runs go. The fuzzer also measures those, on every run, with the
same inputs and random stream (these are measures; no trial joins the
corpus for them unless `--fuzz-keep` asks, see [Corpus
feedback](#corpus-feedback)).

**Assume frontier.** A trial's frontier is the number of distinct
`sea.assume` ops it passed (the approach level of search-based testing).
Without options, after the `fuzz.blocked` lines:

```text
fuzz.frontier: deepest=19 trial=1 end=assume-blocked gate=21 mean=7.66 assumes=22 passed=19 closed=1 unreached=2
fuzz.gate-closed: 21 blocked=7990 check=1 at=kit.cc:67:3 sites=- cond=(xor (eq ? ?) 1)
```

`deepest`: the most any trial passed, the first trial that did, how it
ended and the op that blocked it (`gate=`, an id of `--fuzz-sites`);
`mean` over the trials. Of the assumes, `passed` were passed by some
trial, `closed` blocked trials and were never passed, `unreached` were
never reached. A closed assume is the first lead: the trials get there
and none gets past. A `fuzz.gate-closed` line names each, with its
condition. `check=1` marks an assume directly followed by `assert false`:
in SeaHorn's export form the assume of a check's failure, which a proof
keeps closed ([SeaHorn's export form](#seahorns-export-form)).
`--fuzz-gates` prints one line per assume and `sea.unreachable`, with
the trials that passed it and that it blocked.

**Comparison coverage** (`--fuzz-cmp`). For each `sea.icmp` and
`sea.addr.cmp` a trial evaluates, the bucket of the difference d = lhs -
rhs, computed exactly (as signed values under a signed predicate,
otherwise as unsigned): `-L` (d < -16), `-S` (-16 to -2), `-1`, `0`,
`+1`, `+S` (2 to 16), `+L` (d > 16). A boundary is covered when the runs
have been on both sides of it and at it: code fuzzers see a branch taken
or not, not how close the operands came. `sea.ptr.cmp` (abstract
pointers) is not counted.

```text
fuzz.cmp: 152 reached=121 buckets=299/1064 outcomes=166/304 assume=79/154 branch=214/889 value=6/21
```

`reached`: comparisons evaluated; `buckets` and `outcomes` (true and
false) covered of all; then the buckets by role: comparisons that are the
condition of an assume (directly or through `sea.xor` with true), of a
`sea.cond_br`, or neither; a comparison that is both counts as assume.

A trial an assume of a comparison blocked also has a
branch distance: how far one operand had to move for the condition to
hold (eq: |lhs - rhs|, the short way modulo 2^w; ne: 1; ult: lhs - rhs +
1; ule: lhs - rhs; ugt: rhs - lhs + 1; uge: rhs - lhs; the signed
predicates alike; at most 2^64 - 1). `distance=` on `fuzz.gate-closed`
and `fuzz.gate` is the least over the run (`-`: none measured; the
condition is not a comparison). It says how near the runs came to a
gate they never passed. `ne` gives 1 or nothing, so an assume of
`x != y` (SeaHorn's form of a failed equality check) shows no gradient.
`ne` gives distance 1 even for a gate that can never pass, so
`distance=1` on a closed gate does not mean it was nearly passed.

**Reachable blocks** (`--fuzz-reachable=<file>`). `blocks-total` counts
blocks no input reaches. `sea-vc --reach=blocks` decides which are
reachable; give its output on the same file:

```console
$ sea-vc p.mlir --reach=blocks > reach.txt
$ sea-run p.mlir --fuzz=100000 --fuzz-reachable=reach.txt
fuzz.reachable: covered=6 reachable=7 unreachable=1 unknown=0 unknown-covered=0 unlisted=0
fuzz.uncovered: p.mlir:19:3 at=p.mlir:19:3
```

The ids are those of `--reach-targets=blocks`. Every id of the file must
be a block of the program: the output of another program or lowering is
an error. Run `sea-vc` on the file you fuzz (a program in BMC form, or
the lowered one: `sea-opt --sea-vc-lower`). `unknown`: blocks sea-vc did
not decide, of which `unknown-covered` the trials entered; `unlisted`:
blocks the file does not list. A block a trial entered that the file
calls unreachable is a bug of one of the two tools
(`fuzz.reach-inconsistent`, exit status 1). Not with
`--defer-memory-errors`. `sea-vc --reach` is a solver query per block:
on a large program it can take long (`--reach-rlimit`).

## Local search in the solver

Besides fuzzing, `sea-vc` can run z3's stochastic local search on the
query of the normal solve, the same query the default solve gets (the best
form of it, for example `--lower --widen-memory=8 --seam`):

```console
$ sea-vc p.mlir --lower --widen-memory=8 --seam --z3-tactic=sls-smt --timeout=60
```

`sls-smt` is z3's newer local-search engine: it searches over the Boolean
structure of the query with a bit-vector plugin. It is incomplete: it
answers `sat` or `unknown`, never `unsat`, and an `unknown` is returned
only when `--timeout` expires (exit status 3). Always give a `--timeout`;
z3's resource limit does not reliably stop it. A seed is set in the
tactic: `--z3-tactic='(using-params sls-smt :random_seed 3)'`; z3 rejects
an unknown parameter name there with `result: unknown`. A `sat` is
replayed in the interpreter like any other model.

What to expect today: on small examples with bugs that the default solve
finds in seconds, `sls-smt` found none in 120 seconds over several seeds
and mutants of one function, while the model fuzzer and the default solve
found them. It is worth a try when the default solve times out on a query
that looks satisfiable, or as a cross-check. z3's older `qfbv-sls` tactic
and `sat.local_search` are not recommended.

## Corpus feedback

`--fuzz-keep=<reason>,...` says why a trial joins the corpus:

| reason | a trial is kept when it |
|---|---|
| `blocks` | enters a new block, takes a new edge or passes a `sea.assume` no earlier trial passed (the default, the rule of [Coverage](#coverage)) |
| `cmp` | covers a comparison bucket no earlier trial covered ([Model coverage](#model-coverage); needs the comparison counters, an error with `--fuzz-cmp=false`) |
| `frontier` | passes more distinct `sea.assume` ops than any kept trial (a deeper assume frontier; a kept trial counts when `--fuzz-corpus-max` dropped it) |
| `all` | any of them |

A new pass of one gate is a newly passed assume, so `blocks` covers it.
With the default the corpus and the random stream are those of earlier
versions. The other reasons are untuned options, not recommendations: the
measurements so far are on small programs.

`fuzz.corpus` says why entries were kept:

```text
fuzz.corpus: 92 seeds=0 skipped-lines=0 files=0 dictionary=50 keep=blocks,cmp,frontier kept-blocks=7 kept-cmp=85 kept-frontier=0 dropped=0
```

A trial with several reasons counts under the first in the order blocks,
cmp, frontier; `kept-cmp` is what `cmp` added beyond `blocks`. With
`--fuzz-out` the file of a kept trial names that reason: `cov-<trial>.txt`
(blocks), `cov-cmp-<trial>.txt`, `cov-frontier-<trial>.txt`, and, unless
the rule is the default, its header lists every reason
(`keep=blocks,cmp`).

The corpus cannot grow without bound: each kept trial covers something
first, so the corpus has at most as many entries as there are blocks,
edges, assumes, buckets (7 per comparison) and frontier depths.
`--fuzz-corpus-max=<n>` bounds it further: a kept trial is not added
when the corpus has `n` entries (its coverage still counts and its file
is still written, and it still raises the frontier a `frontier` trial must exceed), and `dropped=` counts these trials.

## Corpus sync

`--fuzz-sync=<dir>,...` lets processes on the same program share inputs,
as the main / secondary instances of AFL do. Every
`--fuzz-sync-period=<s>` seconds (default 5, not tuned) the run lists the
directories, the `--fuzz-out` directories of the other processes, and
runs each `cov-*.txt` file it has not read yet as a trial (in name order
per directory). An imported trial joins this process's corpus by the
same `--fuzz-keep` rule, and is not written to this process's
`--fuzz-out` (it stays where it was found, so it does not echo back).
Failure files are not imported: each process reports its own failures.

```text
fuzz.sync: imported=31 kept=4 skipped=0 polls=12 dirs=15 missing=0 period=5
```

`imported`: the files run as trials (also counted in `fuzz.trials`);
`kept`: of those, the ones that joined the corpus; `skipped`: files that
do not read or parse, or hold a value that does not fit its site (another
program); `missing`: the directories that did not exist at the last
poll. The run's own `--fuzz-out` is never read. The fuzzer writes every
corpus file as a temporary file renamed into place, so a peer reads a
file whole or not at all.

Sync gives up reproducibility: which files a process imports, and when,
depends on the timing of the others, so the same `--seed` no longer gives
the same trials. `utils/fuzz-par.py --sync` wires each process to the
others' directories.

## Failures, memory-safety checks and limits

A trial that fails a `sea.assert` is a failure. With
`--defer-memory-errors`, so is a memory-safety finding (status
`memory-error`, below). The fuzzer stops at the first failure, unless
`--keep-going` is given. Its input is written as `failure-<trial>.txt`.

A trial that stops with status `error` is recorded per operation but does
not stop the fuzzer. There are two kinds:

- A memory-safety check: an access through null, out of an object's
  bounds or to a freed object, a double free, or a free of a pointer that
  is not the start of a heap object. Without `--defer-memory-errors` the
  run stops there, so whether the run was feasible is unknown: an assume
  later on the path might block it. These are `fuzz.memory-check` lines,
  and the first input of each is written as `memory-check-<trial>.txt`.
- A limit: a run the interpreter cannot execute, such as a memory
  operation longer than its limit, `ptr.to_int` in an abstract program,
  a pointer loaded from bytes that do not hold one, or an unsupported
  operation. An access off a word of a word-layout memory (`aligned`,
  `unaligned`) counts here too. Limits are never findings and are never
  deferred. These are `fuzz.limit` lines, with the warning `fuzz.limit`
  (once, then counted), and the first input of each is written as
  `limit-<trial>.txt`.

A broken `align` promise is not an error: the run goes on, with the
warning `interp.align-promise`.

## Deferred memory errors

A run can reach a memory-safety violation and only later reach an
assumption that blocks it. Such a run is infeasible, and the violation is
not a bug. This happens with assumptions placed late: unified assumes
(one assume of all conditions, just before the assertion), prophecy
variables, and programs from other front ends. With
`--defer-memory-errors` (a `sea-run` option, also without `--fuzz`), the
interpreter does not stop at a violation. It records the first one, counts
them all, and goes on:

- a load reads 0, or the null pointer for a pointer load;
- a store, `memset`, `memmove` or `memhavoc` with a violation does
  nothing (none of its cells change);
- a bad `free` does nothing.

The classification assumes that past the first violation any behaviour
is allowed: the access leaves every object, and the model does not say
what happens next. The substitutes pick one continuation, and only decide
whether a later assumption holds. A finding means that the violation is
reached on an exact run of the model up to it, and that some continuation
reaches the end. Under this assumption, a run that a later assume blocks
with the substitutes might pass it with other values, so deferring can miss
a finding; it does not make one up. The substitutes are fixed, so a run
replays.

A finding need not be a counterexample of a VC that gives memory array
semantics past the violation. An out-of-bounds store is dropped here, while
`sea-vc`'s arrays read the stored value back: a program that stores 7 out
of bounds, loads it back, assumes the value is 0 and halts is a finding
here and infeasible there. What happens past a violation is left to the
formal semantics of sea-mlir.

At the end of the run:

| the run | status |
|---|---|
| is blocked by an assume | `assume-blocked`, with `deferred: <n>`: infeasible, not a finding |
| reaches `sea.halt` or a failing `sea.assert` | `memory-error`: a finding, at the first violation, with `reached:` |
| hits the step limit or a limit | its status, with `deferred: <n>`: not classified |

A run that fails an assertion after a violation is reported as the
violation. The assertion's condition may depend on the substitutes, and
the violation came first; once it is fixed, a new run shows the assertion.

```text
status: memory-error
at: loc("p.mlir":8:8)
reached: halted at=p.mlir:11:3
message: memory access out of bounds: offset 9 in stack.1 of size 4
deferred: 1 kind=out-of-bounds at=p.mlir:8:8
```

`at:` is the violation. `reached:` is the status and the location of the
operation that made it reachable. `deferred:` gives the number of
violations in the run and the kind (`null`, `out-of-bounds`,
`use-after-free`, `double-free`, `bad-free`) and location of the first.
`sea-run` exits with 4 on `memory-error`, like `sea-vc`'s 4 (the in-bounds
assumption A-INB does not hold), plus double free and bad free.

Under `--fuzz`, a memory-safety finding is a failure: its file is
`failure-<trial>.txt`, it is replayed and printed as a plain run, and the
fuzzer stops at it unless `--keep-going` is given. It is a
`fuzz.memory-safety` line, and the fuzzer exits with 4. Replay its file
with `--defer-memory-errors`.

Programs whose assumptions are not unified (each assume at its own site)
need deferral less: there, an assume blocks before the accesses after it.

## Output

When there is a failure, the output starts with the first one, as a plain
run on its input: the lines that `sea-run --input-file=<its file>` prints.
The fuzzer replays it before printing and checks that it ends in the same
status at the same operation (`fuzz.replay-mismatch` if not).

Then come the `fuzz.*` lines:

```text
fuzz.failure: 1 trial=32 file=out/failure-32.txt at=p.mlir:12:3 time=0.001
fuzz.trials: 32 seeds=0 time=0.001 execs/s=54659 steps=139 steps/s=211841
fuzz.ranges: 0 value-sites=1 assumes=0 skipped=0 draws=0 mode=mix share=0.5 range-trials=0
fuzz.status: halted=1 assert-failed=1 assume-blocked=30 step-limit=0 error=0
fuzz.coverage: blocks=3 blocks-total=3 edges=3 assumes=1 assumes-total=1 paths=3
fuzz.corpus: 3 seeds=0 skipped-lines=0 files=3 dictionary=6 keep=blocks kept-blocks=3 kept-cmp=0 kept-frontier=0 dropped=0
fuzz.blocked: 30 op=sea.assume at=p.mlir:5:3 sites=0 cond=(eq s0 1234)
```

| line | meaning |
|---|---|
| `fuzz.progress: <s> trials=<n> steps=<n> blocks=<n> edges=<n> paths=<n> assumes=<n> failures=<n> frontier=<n> [cmp-buckets=<n>] corpus=<n>` | `--fuzz-progress`: the totals so far, printed during the run ([Progress](#progress)) |
| `fuzz.failure: <runs> trial=<n> [file=<path>] at=<where> time=<s>` | one per failing `sea.assert`: how many runs failed it, the first trial, its file, the seconds from the start of the trials to it |
| `fuzz.memory-safety: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where> time=<s>` | `--defer-memory-errors`: one per operation with a memory-safety finding (the first violation of a run that reached the end or a failing assert) |
| `fuzz.memory-safety-message: <text>` | the message of the first such run |
| `fuzz.memory-check: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where> time=<s>` | without `--defer-memory-errors`: one per operation that stopped runs at a memory-safety check; `site` is the operation's site, if it has one |
| `fuzz.memory-check-message: <text>` | the message of the first such run |
| `fuzz.limit: <runs> trial=<n> [file=<path>] op=<op> [site=<n>] at=<where> time=<s>` | one per operation that stopped runs at an interpreter limit or a layout error |
| `fuzz.limit-message: <text>` | the message of the first such run |
| `fuzz.trials: <n> seeds=<n> time=<s> execs/s=<n> steps=<n> steps/s=<n>` | trials run (seed runs included), time, trials per second, interpreter steps over all trials and per second ([Progress](#progress)) |
| `fuzz.ranges: <sites> value-sites=<n> assumes=<n> skipped=<n> draws=<n> mode=mix\|edge\|off share=<p> range-trials=<n>` | value sites with a range, of all value sites; the range assumes they come from, the recognized ones not used, and the values drawn from a range; the mode (`mix`: both generators, `edge`: range trials only, `off`: none, with `--fuzz-ranges=false` or a share of 0), the share of range trials asked for, and the trials that were range trials ([Ranges](#ranges)) |
| `fuzz.status: halted=.. assert-failed=.. assume-blocked=.. step-limit=.. error=.. [memory-error=..]` | runs per status (`memory-error` with `--defer-memory-errors`) |
| `fuzz.deferred: <runs> memory-error=.. assume-blocked=.. step-limit=.. error=..` | `--defer-memory-errors`: the runs that went past a violation, by how they ended |
| `fuzz.blocked-before-assert-false: <runs> assumes=<n> deferred=<n>` | when the program has an assume directly followed by `assert false`: the runs blocked at such an assume, the number of them, and of the runs those that went past a deferred memory-safety violation first (`--defer-memory-errors`) ([SeaHorn's export form](#seahorns-export-form)); these runs are also counted as `assume-blocked` |
| `fuzz.coverage: blocks=<n> blocks-total=<n> edges=<n> assumes=<n> assumes-total=<n> paths=<n> [paths-capped=<cap>]` | blocks entered, edges taken (the entry counts as one), `sea.assume` ops passed at least once, distinct paths ([Coverage](#coverage)) |
| `fuzz.corpus: <entries> seeds=<files> skipped-lines=<n> files=<n> dictionary=<n> keep=<reasons> kept-blocks=<n> kept-cmp=<n> kept-frontier=<n> [max=<n>] dropped=<n>` | corpus entries at the end, seed files read, lines of saved runs that are not inputs, files written, dictionary size; the keep rule, the trials kept per first reason, the bound and the kept trials it dropped ([Corpus feedback](#corpus-feedback)) |
| `fuzz.sync: imported=<n> kept=<n> skipped=<n> polls=<n> dirs=<n> missing=<n> period=<s>` | `--fuzz-sync`: the peers' files run, kept, skipped ([Corpus sync](#corpus-sync)) |
| `fuzz.blocked: <runs> op=<op> at=<where> [sites=<n,...> cond=<expr>]` | one per `sea.assume` or `sea.unreachable` that blocked runs, most first; for a `sea.assume`, its condition over the input sites (see [For agents](#for-agents)) |
| `fuzz.frontier: deepest=<n> trial=<n> end=<status> [gate=<id>] mean=<x> assumes=<n> passed=<n> closed=<n> unreached=<n>` | the assume frontier ([Model coverage](#model-coverage)) |
| `fuzz.gate-closed: <id> blocked=<n> [distance=<d>\|-] [check=1] at=<where> [sites=<n,...> cond=<expr>]` | one per `sea.assume` that blocked trials and that no trial passed |
| `fuzz.gate: <id> op=<op> passed=<n> blocked=<n> [distance=<d>\|- measured=<n>] [check=1] at=<where>` | `--fuzz-gates`: one per `sea.assume` and `sea.unreachable`; `measured`: the blocked trials with a distance |
| `fuzz.cmp: <n> reached=<n> buckets=<hit>/<total> outcomes=<hit>/<total> assume=<hit>/<total> branch=<hit>/<total> value=<hit>/<total>` | `--fuzz-cmp`: comparison coverage |
| `fuzz.reachable: covered=<n> reachable=<n> unreachable=<n> unknown=<n> unknown-covered=<n> unlisted=<n>` | `--fuzz-reachable`: blocks covered of the reachable ones |
| `fuzz.uncovered: <id> at=<where>` | `--fuzz-reachable`: one per reachable block no trial entered |

`<where>` is a location as in the `assert-at:` line: the source position,
then `inlined-at=` and `name=` when there are any. With `--format=jsonl`,
each line is a `stat` record ([Output formats](output-formats.md)).

The `fuzz.blocked` lines show which assumptions stop the runs. A harness
whose runs are almost all blocked by one assume tests little beyond it:
that assume is the one to look at, or to seed past with a corpus entry.

## SeaHorn's export form

SeaHorn exports a check as an assume of its failure followed by
`assert false` and `sea.halt`: reaching the assertion is the failure. A
run in which the check holds is blocked at that assume, so `fuzz.status`
shows `halted=0` and the runs that passed the check are among the
`assume-blocked` ones. The line

```text
fuzz.blocked-before-assert-false: 7445 assumes=2 deferred=0
```

counts the runs blocked at an assume that is directly followed by
`sea.assert` of a constant false: with nothing between them but pure
operations (the constant) and unconditional `sea.br` hops. In the export
form these are the runs that passed a check and ended there, the valid
trials, unless a deferred memory-safety violation came before
(`deferred=`); the `fuzz.blocked` lines show which of the assumes blocked them.
The line says what the program's structure is; whether such a block is a
passed check is a reading of the export form. The line is printed only for
programs that have such an assume. `fuzz.status` is unchanged.

## Reach coverage

`--reach-targets` (`=checks`, the default, or `=blocks`) reports which
reach targets the runs reached: the targets of `sea-vc --reach`, with the
same ids ([Reach](output-formats.md#reach)). A target is reached when a run
is about to execute its op (a block: its first op), every earlier assume
having held; what the run does after that does not matter. Run on the
lowered program (`sea-opt --sea-vc-lower` with `sea-vc`'s options) to get
the targets of `sea-vc --lower --reach`.

```text
reach: p.mlir:5:3 reached op=sea.assume by=trial:2 witness=w/reach-0.txt at=p.mlir:5:3
reach: p.mlir:9:3 not-reached op=sea.assert at=p.mlir:9:3
reach.summary: targets=2 reached=1 not-reached=1 by-seed=0 by-trial=1 witnesses=1
```

| line | meaning |
|---|---|
| `reach: <id> reached\|not-reached op=<op> [by=seed:<file>\|trial:<n>\|run] [witness=<file>] at=<where>` | one per target, in program order; `by`: the first run that reached it, a `--corpus` file, a trial (1-based, seed runs counted), or `run` for a plain run |
| `reach.summary: targets=<n> reached=<n> not-reached=<n> [by-seed=<n> by-trial=<n> witnesses=<n>]` | the totals (`by-*` and `witnesses` with `--fuzz`) |
| `assume.addr0-is-null: uses=<n>`, `assume.memhavoc-null-noop: uses=<n>` | the deciding runs' uses of the assumptions (each run once) |

With `--fuzz`, the lines follow the `fuzz.*` lines. The corpus files run
first, so a seed corpus and the witnesses of an earlier run count.
`--reach-witness=<dir>` writes the inputs of the first trial that reached
each target to `<dir>/reach-<k>.txt` (k: the target's position); a target a
corpus file reached has that file as its witness. The fuzzer stops at the
first failing assertion unless `--keep-going`; for coverage, use it. Not
with `--defer-memory-errors`: past a violation the substitutes make up the
run, so it is not an execution of the program. `not-reached` says only that
no run reached the target; `sea-vc --reach` decides it.

## For agents

Two outputs let a program outside `sea-mlir` (an agent's script, say) see
why runs are blocked and choose inputs to get past them, without reading
the IR.

`--fuzz-sites` lists the input sites of the program, in program order,
and exits without running it:

```console
$ sea-run placed.mlir --fuzz-sites
fuzz.site: 0 callee=region.0 width=8 input=memory op=sea.mem.nondet at=placed.mlir:50:11 name=sm15
...
fuzz.site: 6 callee=nd_size_t width=64 input=value op=sea.nondet at=seahorn/lib/byte_buf_helper.c:19:18 name=_4
fuzz.site: 7 callee=nd_size_t width=64 input=value op=sea.nondet at=seahorn/lib/byte_buf_helper.c:20:18 name=_5
fuzz.site: 8 callee=nd_malloc_is_fail width=1 input=value op=sea.nondet at=seahorn/lib/sea_allocators.c:11:10 name=_13
...
fuzz.assume: 0 op=sea.assume at=seahorn/lib/byte_buf_helper.c:21:5 sites=6,7 cond=(ule s6 s7)
fuzz.assume: 1 op=sea.assume at=seahorn/lib/byte_buf_helper.c:22:5 sites=7 cond=(ult s7 11)
...
fuzz.sites: 17 value=7 memory=10 assumes=10 unreachables=0
```

(the vcc job `byte_buf_append`, lowered as in
[Programs that do not run](#programs-that-do-not-run)).


| field | meaning |
|---|---|
| value | the site number: the key of `--input` and corpus assignments |
| `callee` | the nondet function (`sea.nondet`, `sea.struct.nondet`), the memory's name (`sea.mem.nondet`), or `-` (`sea.memhavoc`) |
| `width` | the value's width in bits; for a memory, the width of a cell |
| `input` | `value`: one value per run (`<site>=<v>`); `memory`: one value per cell read (`<site>@<addr>=<v>`) |
| `op` | the operation |
| `at` | its location, as in `<where>` (with `inlined-at=` and `name=` when there are any) |

Then comes one `fuzz.assume` line per operation that can block a run:
every `sea.assume`, with its condition over the input sites (`sites` and
`cond`, as in the `fuzz.blocked` lines below), and every
`sea.unreachable`, without one. They are in program order, and the number
after `fuzz.assume:` is the position in that order: `--fuzz-serve`
names assumes by it. So a strategy can be planned from the conditions
before any run.

Then comes one `fuzz.range` line per value site with a range, the range
`--fuzz` draws from ([Ranges](#ranges)):

```text
fuzz.range: 12 width=64 lo=1 span=63 bounds=- assumes=2
fuzz.range: 28 width=64 lo=0 span=- bounds=s12+s15+s18+18446744073709551615 assumes=19
```

| field | meaning |
|---|---|
| value | the site |
| `width` | its width |
| `lo` | the low end, in unsigned decimal |
| `span` | the range is `lo` plus 0 to `span`, modulo 2^width; `-`: no constant bound |
| `bounds` | `,`-separated sums (sites `s<N>` and an unsigned constant, joined by `+`) that also bound the span, evaluated in each trial; `-`: none |
| `assumes` | the ids of the assumes the range comes from |

The last line, `fuzz.sites: <n> value=<n> memory=<n> assumes=<n> unreachables=<n>`,
counts the sites, the `sea.assume` ops and the `sea.unreachable` ops (the
`fuzz.assume` lines are both); `assumes` is the `assumes-total` of
`fuzz.coverage` and the `assumes_total` of `--fuzz-serve`. `0` sites means
the program reads no input. `sea.ptr.nondet` is not listed: it
makes a fresh object and takes no input. `--fuzz-sites` refuses the
options that would have no effect (`--fuzz`, `--fuzz-time`, `--input`,
`--input-file`, `--trace`, `--seed`, `--max-steps`,
`--defer-memory-errors`).

Each `fuzz.blocked` line of a `sea.assume` ends with the assume's
condition written over the input sites:

```text
fuzz.blocked: 3054 op=sea.assume at=seahorn/lib/byte_buf_helper.c:21:5 sites=6,7 cond=(ule s6 s7)
fuzz.blocked: 2837 op=sea.assume at=seahorn/lib/byte_buf_helper.c:22:5 sites=7 cond=(ult s7 11)
fuzz.blocked: 922 op=sea.assume at=seahorn/jobs/byte_buf_append/aws_byte_buf_append_harness.c:13:5 sites=- cond=?
```

Read it as: the assume holds when site 6 is at most site 7 (unsigned),
and when site 7 is below 11. Corpus entries with `7` in 0..10 and `6` at
most `7` pass both. The third assume's condition is a value merged from
several paths, so the walk stops at once.

`cond` is an s-expression:

- `s<N>`: the value of input site N;
- a number: a constant, in unsigned decimal (so -1 in 8 bits is 255);
  a placed object's base address is a constant too;
- `(<pred> a b)`: a comparison, with the predicate of `sea.icmp` (`eq`,
  `ne`, `ult`, `ule`, `ugt`, `uge`, `slt`, `sle`, `sgt`, `sge`); it
  compares at the width of its operands;
- `(<op> a ...)`: an operation whose value the text fully shows, named
  as in the IR without `sea.`: the arithmetic and bitwise operations,
  min and max, the overflow tests, `concat`, `select`, `struct.create`,
  the `addr.*` operations, and `ptr.add`, `ptr.diff`, `ptr.null`. The
  `nsw` and `nuw` promises show as `.nsw` and `.nuw` (`(add.nsw a b)`).
  Operations that change the width carry the result width:
  `(zext:64 a)`, `(trunc:8 a)`, `(extract:8 a <low>)`;
- `(struct.extract s <index>)`, `(struct.insert s v <index>)`,
  `(global @<name>)`, `(func_addr @<name>)`, and `(ptr.index <base>
  <indices> strides=<s,...> offset=<n>)` with `inbounds`, `nusw`, `nuw`
  when set;
- `(load:<w> s<N>@<addr>)`: a `w`-bit load from the memory of site N, a
  `sea.mem.nondet` whose only users are loads, so the cells are still
  the inputs; it reads the cells at `addr` and after, low byte first;
- `?`: where the walk stopped.

`sites` lists the sites that `cond` names, or `-` for none. `cond` is the
last field and runs to the end of the line, because it holds spaces; in
`--format=jsonl`, `sites` and `cond` are string fields.

The condition is computed once per blocking assume, from the program text,
when the report prints; the trials do not pay for it. It is a backward walk
from the assume's operand, not a symbolic execution. It stops with `?` at:

- any operation not listed above: other memory reads (a load from a
  memory that an update uses, even in place), operations with effects
  such as allocations, and operations whose value depends on more than
  their operands (`ptr.base`, `ptr.dereferenceable`, `ptr.to_int`);
  also a listed operation with an attribute the text does not show;
- a block argument with more than one incoming value (a value merged
  from several paths, such as the result of `a && b` in C); a block
  argument with a single incoming value is followed;
- an operation more than 6 levels below the condition (sites and
  constants are shown at any depth);
- any node past the 40th.

The condition says nothing about the path to the assume: a run that does
not reach it is not blocked by it. `cond=0` is an assume of a constant
false: no input passes it, so the code after it is unreachable.

## Driving the fuzzer from Python

`sea-run p.mlir --fuzz-serve` lets a program outside `sea-mlir` choose the
inputs. It parses the program once, then reads one JSON request per line
on stdin and writes one JSON response per line on stdout, flushing each.
End of input ends it, after a summary line. Each request is one run, a
trial of `--fuzz` on the values the request gives, and the coverage is
kept across requests, so a response says what that run reached first.

`utils/fuzz-driver.py` (Python standard library only) is a reference
driver to copy: a `Server` class that starts `sea-run --fuzz-serve`, sends
requests and returns the parsed responses, and a `main()` with one simple
strategy.

`--fuzz-serve` takes `--program`, `--max-steps`, `--defer-memory-errors`,
`--missing` (the default of the requests) and `--seed` (the random values
of requests without a seed of their own). It refuses the options of
`--fuzz` and of single runs, and `--format`: it always writes JSON lines.

### Requests

```json
{"inputs": {"7": 10, "6": "3", "0@4096": "0xff"}, "missing": "random", "seed": 1}
```

| field | meaning |
|---|---|
| `inputs` | values by site, `"<site>"` or `"<site>@<addr>"` (`"<site>@<object>+<offset>"` for an abstract memory), as in `--input`; a value is an integer or a string (decimal, negative, `0x` hex) and must fit the site's width. A cell the run never reads is ignored, as with `--input`. The object names of abstract memories (`stack.1`) are those in the `input` and `reads` of earlier responses |
| `missing` | what a site or cell the request does not set gets: `"zero"`, `"random"`, or `"error"` (the run stops with status `error`, as in a plain run); default: the server's `--missing` |
| `seed` | the seed of this run's random values; without it, the server's stream continues |
| `trace_sites` | `true`: the response lists every value the run read (`reads`) |
| `id` | any JSON value, echoed in the response |

All fields are optional. As in `--fuzz`, a site has one value per run.

### Responses

```text
{"kind":"response","request":3,"status":"assume-blocked","loc":"loc(...)",
 "blocked":3,"blocked_at":"seahorn/jobs/byte_buf_append/aws_byte_buf_append_harness.c:13:5",
 "new":{"blocks":[5],"edges":[[3,5],[5,6]],"assumes":[]},
 "coverage":{"blocks":36,"blocks_total":190,"edges":37,"assumes":6,"assumes_total":10},
 "input":["6=4","7=4","8=1"],"steps":228,"text":"status: assume-blocked\nat: loc(...)"}
```

(shown on several lines; it is one line.)

| field | meaning |
|---|---|
| `request` | the request's number, counting non-blank lines from 1 |
| `id` | the request's `id`, if it has one |
| `status`, `loc`, `at`, `op`, `path`, `reached`, `reached_at`, `message`, `deferred`, `deferred_kind`, `deferred_at` | the fields of the `status` record of `sea-run --format=jsonl` ([Output formats](output-formats.md)): `loc` is the `at:` line (where the run stopped), `at`, `op` and `path` are `assert-at`, `assert-op` and `assert-path`; the `deferred*` and `reached*` fields come with `--defer-memory-errors` |
| `blocked`, `blocked_at` | for a blocked run, the id of the blocking `sea.assume` or `sea.unreachable` (the number of its `fuzz.assume` line in `--fuzz-sites`) and its location; join on the id, not on the location, which inlined code can share between several operations and which may carry `inlined-at=` |
| `new` | what no earlier request reached: `blocks` (a block's position in the program body; the entry is 0), `edges` (`[from, to]`; from `-1` is the entry of the run), `assumes` (`sea.assume` ids that held) |
| `coverage` | the totals over all requests so far, as in `fuzz.coverage`; `assumes` and `assumes_total` count `sea.assume` ops only, as `assumes=` of `fuzz.sites` |
| `input` | every value the run read, in corpus syntax: written one per line, they are a corpus file that `sea-run --input-file` replays (with `--defer-memory-errors` for a `memory-error`) |
| `reads` | with `trace_sites`: each read in order, `{"site", ["addr",] "width", "value", "origin"}`, origin `input`, `zero` or `random` |
| `steps` | operations executed |
| `text` | the status lines a plain run prints |

A bad request gets `{"kind":"error","request":<n>,"error":"<why>"}` and
runs nothing: a line that is not JSON, an unknown field, a site the
program does not have, a value that does not fit its site, or a cell of a
value site (or the reverse). The server goes on with the next line. At
the end of input it writes `{"kind":"summary","requests":<n>,"errors":<n>,"coverage":{...}}`
and exits with 0.

### The reference driver

```console
$ cat strategy.json
{"sites": {"7": {"range": [0, 10]},
           "6": {"range": [0, "s7"]},
           "11": {"range": [0, 10]},
           "8": {"choice": [0, 1]}},
 "missing": "random"}
$ python3 utils/fuzz-driver.py placed.mlir --strategy strategy.json --requests 20000
requests: 20000 kept=16 resampled=0
status: assume-blocked=20000
coverage: blocks=145 blocks_total=190 edges=166 assumes=7 assumes_total=10
blocked: 8990 assume=3 at=seahorn/jobs/byte_buf_append/aws_byte_buf_append_harness.c:13:5
blocked: 4971 assume=5 at=seahorn/jobs/byte_buf_append/aws_byte_buf_append_harness.c:20:5
...
```

A strategy gives each listed site a `range` (inclusive) or a `choice`.
A bound is an integer, or another site's value plus or minus an integer:
`"s7"`, `"s7 + 1"`, `"s6 - 1"`; that site is sampled first. A bound that
names a site the strategy does not list, a cycle of bounds, or any other
bound text is an error. An empty range (`lo > hi`, such as `[0, "s6 - 1"]`
when site 6 is 0) discards the whole sample, and the driver draws again;
the `resampled` count says how often (1000 empty samples in a row is an
error). Other sites get `missing`. The driver keeps the inputs that reach new coverage, stops
at the first failure and writes its input as a corpus file (`--out`).
Here, on the `byte_buf_append` job, the strategy is read off the
`fuzz.assume` conditions `(ule s6 s7)`, `(ult s7 11)` and `(ult s11 11)`:
every run passes them, and the next blockers are the harness's assumes,
whose conditions (`?`) depend on memory.

Adding `"15": {"range": [0, "s6 - 1"]}` for `(ult s15 s6)`
(assume 7) gets every run past assume 7 too: 8 of the 10 assumes pass.
The runs with site 6 at 0 are drawn again, so the paths of an empty
buffer are no longer run (125 blocks instead of 145). The next blockers
are assumes 3, 5 and 9 of the harness and assume 8, `(ult s16 ?)`, whose
bound is not an input.

To write another strategy, keep `Server` and replace the loop: mutate kept
inputs, solve one blocking condition with z3, or anything else.
Throughput is bounded by the round trip of one request: a Python driver
gets fewer runs per second than `--fuzz`, which runs in one process.

## Corpus files

A corpus file holds one input: one assignment per line.

```text
# a comment
0=1234
2@16=7
3@heap.4+8=0xff
```

An assignment is in the `--input` syntax: `<site>=<value>` for a nondet
site, `<site>@<addr>=<value>` for a memory cell, `<site>@<object>+<offset>=<value>`
for a cell of an abstract memory. The `nondet:` and `memread:` lines of a
`sea-run` or `sea-vc` output are read too, and the other `key: value` lines
of such an output are skipped, so a saved run is a seed:

```console
$ sea-run p.mlir --input=0=1234 > seed.txt
$ sea-run p.mlir --fuzz=10000 --corpus=seed.txt
```

Blank lines and `#` comments are skipped. Any other line is an error, as is
a value that does not fit its site.

## Reproducibility

The same program, `--seed`, corpus and options give the same trials and the
same output, except the `time`, `execs/s` and `steps/s` fields and the
`fuzz.progress` lines. With `--fuzz-time`, the
number of trials depends on the machine's speed, so the result is
reproducible only up to where the time ran out: give `--fuzz` for a fixed
run. With `--fuzz-sync` a run is not reproducible ([Corpus
sync](#corpus-sync)).

## Logs

`--log=fuzz.run` shows the start and end of the run; `--log=fuzz.run:2`
also shows each trial that reached new coverage ([Logging](logging.md)).

## Programs that do not run

The fuzzer runs what `sea-run` runs. An abstract program (pointers into
unplaced objects) runs directly, but stops on operations that need
addresses, such as `ptr.to_int`. Such a program can be fuzzed after the
lowering `sea-vc --lower` runs, which places the objects:

```console
$ sea-opt p.mlir --mlir-print-debuginfo --sea-apply-tracking \
    --sea-scalarize-structs --sea-drop-fresh-memhavoc --sea-lower-tracking \
    --sea-lower-fat-pointers --sea-place-static --sea-vc-lower -o placed.mlir
$ sea-run placed.mlir --fuzz=10000
```
