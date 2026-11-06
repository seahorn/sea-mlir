# Reducing sea programs (sea-reduce, utils/reduce-test.py)

A crash, a configuration disagreement or a blowup on a 14k-line SeaHorn
export is hard to debug and useless as a regression test. `sea-reduce`
shrinks the input while an *interestingness test* keeps saying the
symptom is still there; `utils/reduce-test.py` is that test for the usual
questions, and runs the reduction loop.

The result is not equivalent to the input: it is a smaller program with
the same symptom, nothing more. A reduced performance case says where to
look, not why.

## Quick start

```bash
# a verdict disagreement between two sea-vc configurations
utils/reduce-test.py reduce --mode verdict-differs \
    --a="--lower" --b="--lower --seam" in.mlir -o small.mlir --log t.log

# a slowdown: configuration A over 12 s while B stays under 10 s
utils/reduce-test.py reduce --mode slow --metric time \
    --a="--lower --seam --widen-memory=4 --vcgen-only --seam-row-group=0" \
    --b="--lower --seam --widen-memory=4 --vcgen-only --seam-row-group=1" \
    --a-over 12 --b-under 10 --sea-vc build-release/bin/sea-vc \
    in.mlir -o small.mlir --log t.log

# a crash (signal) or a diagnostic
utils/reduce-test.py reduce --mode crash --config="--lower --seam" \
    --signal any in.mlir -o small.mlir
utils/reduce-test.py reduce --mode crash --config="--lower" \
    --match "vc.mem-op-symbolic-length" in.mlir -o small.mlir

# a specific result (and exit status)
utils/reduce-test.py reduce --mode result --config="--lower --seam" \
    --result sat --status 2 in.mlir -o small.mlir
```

`reduce` checks that the input is interesting, writes the test script,
runs `sea-reduce` rounds until the file stops shrinking, tests the output
again (`reduce.recheck`: a timing case can be noise) and prints
`reduce.input-lines / -ops`, `reduce.output-lines / -ops`,
`reduce.time`. `--log` gets one line per candidate (interesting, size,
measurements); `tail -f` it to watch progress. Answers are cached by file content
(`<work>/cache`): reduction-tree tests many variants identical to their
parent (no pattern matched the ops outside the kept range), and those
cost nothing then. Use a Release sea-vc for
performance cases: the budgets are calibrated on the binary that runs.

Arguments that start with `--` take `=`: `--a="--lower --seam"`.

## The interestingness modes

Every mode runs `sea-opt <file>` first: a candidate that does not parse
or verify is uninteresting. sea-vc runs get `<file>` first, then the
configuration's arguments; all runs are capped by `--timeout` (seconds,
default 600) and `--mem-cap` (GB of address space, RLIMIT_AS, default 8).

| mode | interesting when | not interesting |
|---|---|---|
| `crash` | the tool (`--tool`, default sea-vc) dies by `--signal <n>` (or `any`) and/or its output matches `--match <regex>` | it verifies but neither happens; a timeout |
| `verdict-differs` | A and B both print `result: sat` or `unsat` and the results differ (`--a-result` fixes A's) | either side errors (status 1), dies, times out, or says unknown |
| `slow` | B stays under `--b-under` and A exceeds `--a-over` on `--metric`; optionally A >= `--min-ratio` x B | B over its budget or failing; A failing for any reason but the budget |
| `result` | the configuration prints `result: <--result>` (and exits `--status`) | an error, a signal, a timeout, another result |

`slow` metrics: `time` (seconds; B runs with timeout `b-under`, A is
killed at `a-over`, which counts as over), `mem` (GB; A runs under
RLIMIT_AS = `a-over`: killed, `mem-limit: hit` or an allocation failure
count as over; B's peak RSS must stay under `b-under`), or a counter name
as `--seam-stats` prints it (`seam.rule.bv.ite-merge`; deterministic, no
noise, but needs `--seam-stats` in both configurations; a counter B does
not print is 0).

Choosing the predicate matters more than the reducer: "sat" alone reduces
to `assert false`, and "unsat" alone to an inconsistent assume. Tie the
test to the discrepancy with a configuration comparison, diagnostic tag,
or mechanism counter.

To write a test by hand: `sea-reduce` runs `<test> [test-arg...] <file>`;
exit status nonzero = interesting, 0 = not (upstream
`mlir/lib/Reducer/Tester.cpp`). `reduce-test.py script <mode options> -o
t.sh` writes such a script for a mode.

## Candidate outcomes, and whether the patterns make valid sea-vc input

Every candidate gets one outcome, logged with its stage and diagnostic
kind (`--log`, default `<work>/candidates.log`):

| outcome | meaning |
|---|---|
| `verifier-rejected` | the MLIR verifier refused it inside sea-reduce (the test never ran); counted per stage from `sea-reduce.verifier-rejected` |
| `verify-failed` | `sea-opt` refused it (should not happen after the above) |
| `sea-vc-error` | sea-vc stopped with an error: BMC form, a `vc.*` input-form error, a pass postcondition; `kind` is the `[tag]` or the message head |
| `crashed` / `timeout` | a signal / the safety timeout |
| `uninteresting` / `interesting` | it ran, and the symptom is absent / present |

Only `interesting` is kept; every other outcome is uninteresting in every
mode (crash mode excepted for the one diagnostic it matches;
`test/Reduce/form-errors.mlir`). `reduce` prints the table per stage
(`reduce.stage:` lines, then `reduce.kind:` lines for the error kinds);
`reduce-test.py summary <log>` prints it for any log. Stages:
`global:<pattern>` (a pattern everywhere at once), `cleanup[:part]`,
`tree:<pattern>` (sea-reduction-tree with that pattern only),
`tree:delete` (upstream reduction-tree with no pattern: op-range
deletion only; nearly all its `verifier-rejected` candidates). `cached`
counts repeats answered from the cache; the outcome columns count each
candidate once. An opt-reduction stage first re-tests its input
(upstream), which counts as one candidate.

`reduce-test.py audit <mode options> in.mlir --samples n` asks the
pattern question directly: each pattern applied once at n sampled sites
(`sea-reduce-apply{pattern=p op-index=i}`; `report=true` lists the
sites) and each candidate classified. A pattern whose candidates sea-vc
refuses is broken. In a Release build, on the h2fc export
(`--lower --seam --widen-memory=4 --vcgen-only`) 101 candidates
(memory-bypass 20, drop-assume 1, select-branch 20, load-to-nondet 20,
forward-operand 20, zero-const 20; erase-dead: no site), all accepted;
on the inputs of test/Reduce (`--lower`, with and without `--seam`) 106,
all accepted.

## What sea-reduce does

`sea-reduce` is upstream `mlir-reduce` (`mlirReduceMain`) with the sea
dialect, every sea and upstream pass, and a
`DialectReductionPatternInterface` on the sea dialect:

- `--reduction-tree='traversal-mode=0 test=t.sh'`: (1) delete ranges of
  ops (binary splitting; a range that leaves the program invalid is
  rejected by the verifier without running the test), (2) apply the sea
  patterns to ranges of ops. One test per variant.
- `--sea-reduction-tree='test=t.sh pattern=<p>,...'` (ours,
  `lib/Reduce/SeaReductionTree.cpp`): the same single-path search over
  ranges of ops, built on upstream's ReductionNode and Tester, with the
  sea patterns only: no op-deletion phase (in a 14k-op CFG upstream's
  deletion phase mostly makes candidates the verifier rejects, and the
  few that verify remove a handful of ops per test: hours), folding off,
  and a variant no pattern changed is not tested.
- `--opt-reduction-pass='opt-pass=<pipeline> test=t.sh'`: run a pass
  pipeline once; kept only if the result is interesting and smaller.

The sea patterns (`lib/Reduce/ReductionPatterns.cpp`), each keeping the
program verifying (unique nondet sites, typed block arguments,
terminators):

| pattern | rewrite |
|---|---|
| `memory-bypass` | store / memset / memmove / memhavoc / reset_modified -> its incoming memory |
| `drop-assume` | `sea.assume` erased |
| `select-branch` | `cond_br` -> `br` to one successor (the one not ending in `sea.unreachable`, else the true one) with its block arguments |
| `load-to-nondet` | load -> `sea.nondet "sea.reduce" [fresh site]` (pointer: `sea.ptr.nondet`) |
| `forward-operand` | a value op -> its first operand of the same type (add -> lhs, ptr.add -> ptr, select -> true value) |
| `zero-const` | an integer op without such an operand (icmp, casts) -> `sea.const 0` |
| `erase-dead` | an op with unused results (nondets and memory updates too) |

As passes: `sea-reduce-apply{pattern=<name>,... limit=<n>}` (the first n
matches, 0 = all; in sea-opt too) and `sea-reduce-cleanup` (dead ops,
unreachable blocks, unused block arguments, br chains merged; options
`dead-ops=false` / `blocks=false` keep one part).
`reduce-test.py reduce` runs per round: each of drop-assume,
select-branch, memory-bypass, load-to-nondet everywhere at once (one test
each), cleanup (whole, then each part), sea-reduction-tree once per
pattern, cleanup, upstream reduction-tree with no pattern (op deletion;
`--no-delete` skips it), cleanup.
Unreachable blocks can carry the symptom: sea-vc encodes every block of
the program, reachable or not (seen on the h2fc case: removing the
blocks select-branch disconnected removed the blowup).

## Limits

- `select-branch` applied everywhere at once reaches only the cond_brs
  still reachable: the greedy driver skips ops in blocks an earlier
  rewrite disconnected.
- reduction-tree applies one pattern per op (the highest benefit that
  matches): select-branch never tries the other successor; zero-const is
  not tried where forward-operand applies.
- The reduced program need not be in BMC form for `sea-vc` without
  `--lower`; reduce the input of the configuration you run (an abstract
  program with `--lower`, or a lowered one without).
- Op-range deletion rarely succeeds in a CFG (a range usually contains a
  terminator or a definition still used); the patterns do most of the
  work, and each accepted variant costs a full test run: a 14k-op program
  with a 20 s test takes hours.
- Not yet: aggregate (struct) simplification, offset / length
  simplification, block-argument merging, memory-object removal
  (globals / allocas are erased only when unused).
