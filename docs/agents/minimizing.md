# Minimizing a problem

A crash, a disagreement between two configurations, or a blowup on an
export of thousands of lines is hard to study and too large for a
regression test. Reduce it first. `sea-reduce` shrinks a program while an
*interestingness test* keeps reporting the symptom. `sea-reduce-test`
provides tests for the usual symptoms and runs the reduction loop.

The result is a smaller program with the same symptom. It is not
equivalent to the input. A reduced performance case shows where to look,
not why.

## Tie the test to the symptom

The test decides what is kept, so it must describe the symptom exactly. A
test that asks only for `sat` is satisfied by a program that asserts
false. Reducing the HPACK mutant from [Firedancer: HPACK](../tutorials/firedancer.md)
with that test:

```console
$ sea-reduce-test reduce --mode result --config="--lower --seam --widen-memory=4" \
    --result sat mut.mlir -o small.mlir
reduce.input-lines: 323
reduce.input-ops: 292
...
reduce.output-lines: 7
reduce.output-ops: 3
...
reduce.recheck: interesting (result=sat status=0 signal=None timeout=0 time=0.08 rss=0.045)
...
$ cat small.mlir
```

The reduced program is:

```mlir
module {
  sea.program @main {
    %0 = sea.const false
    sea.assert %0
    sea.halt
  }
}
```

The reduced program still answers `sat`, but it no longer contains the
mutant. In the same way, a test for `unsat` alone reduces to an
inconsistent assumption. Tie the test to what differs: two configurations,
a diagnostic kind, or a counter of the mechanism.

| symptom | mode and options |
|---|---|
| two configurations disagree | `--mode verdict-differs --a="<args>" --b="<args>"` |
| a crash or a diagnostic | `--mode crash --config="<args>" --signal any` or `--match <regex>` |
| a result with a given exit status, such as a replay mismatch | `--mode result --config="<args>" --result sat --status 2` |
| a slowdown | `--mode slow --metric time --a="<args>" --b="<args>" --a-over <s> --b-under <s>` |
| a counter that blows up | `--mode slow --metric seam.rule.bv.ite-merge ...`, with `--seam-stats` in both configurations |

Prefer a counter to time when the symptom shows in one. Counters are
deterministic, so the reduction does not chase noise. For timing cases,
use a Release build: the budgets are calibrated on the binary that runs.
`--log <file>` writes one line per candidate; follow it with `tail -f`.

Arguments that start with `--` are passed with `=`, as in
`--a="--lower --seam"`.

## When a smaller query is slower

A change that shrinks the query can make the solver slower. Do not revert
it on one timing. Find the mechanism: raise the solver's verbosity
(`--z3-param=verbose=<n>`), and run several random seeds to separate a
real effect from variance.

## Report

A bug report has the version line, the exact command line with every
`--config`, the exit status, and the reduced program. The reducer's
modes, outcomes and patterns are on [The reducer](../reference/reducer.md).
