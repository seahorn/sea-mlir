# The reducer

`sea-reduce` shrinks a sea program while an interestingness test keeps
reporting a symptom. `sea-reduce-test` (`utils/reduce-test.py` in a source
tree) writes that test for the usual symptoms and runs the reduction loop.
The result is a smaller program with the same symptom; it is not
equivalent to the input. For the workflow, see
[Minimizing a problem](../agents/minimizing.md).

## sea-reduce-test

```text
sea-reduce-test reduce <mode options> <in.mlir> -o <out.mlir> [--rounds 4] [--work <dir>] [--log <file>]
sea-reduce-test script <mode options> -o t.sh     # a test script for sea-reduce
sea-reduce-test test   <mode options> <file>      # exit 1: interesting
sea-reduce-test summary <log>                      # the outcome table of a log
```

`reduce` checks that the input is interesting, writes the test, runs
rounds of `sea-reduce` until the file stops shrinking, tests the output
again (`reduce.recheck`; a timing case can be noise), and prints
`reduce.input-lines`, `reduce.output-lines`, `reduce.time` and per-stage
counts. Answers are cached by file content.

Common options: `--sea-vc`, `--sea-opt`, `--sea-reduce` (default: the
environment variables `SEA_VC`, `SEA_OPT`, `SEA_REDUCE`, then the tools
next to the script, then `PATH`); `--timeout <s>` (default 600) and
`--mem-cap <GB>` (default 8), the limits of every run. Arguments that
start with `--` are passed with `=`: `--a="--lower --seam"`.

## Modes

Every mode first runs `sea-opt` on the candidate: a candidate that does
not parse or verify is not interesting. `sea-vc` runs get the file first,
then the configuration's arguments.

| mode | interesting when | not interesting |
|---|---|---|
| `crash` | the tool (`--tool`, default `sea-vc`) dies by `--signal <n>` (or `any`), and/or its output matches `--match <regex>` | it verifies but neither happens; a timeout |
| `verdict-differs` | configurations A and B both print `sat` or `unsat`, and the results differ (`--a-result` fixes A's) | either side errors, dies, times out, or says `unknown` |
| `slow` | B stays under `--b-under` and A exceeds `--a-over` on `--metric` (optionally A is at least `--min-ratio` times B) | B over its budget or failing; A failing for any other reason |
| `result` | the configuration prints `result: <--result>` (and exits with `--status`) | an error, a signal, a timeout, another result |

The `slow` metrics are `time` (seconds), `mem` (GB; A runs under an
address-space limit of `a-over`), or a counter name as `--seam-stats`
prints it, such as `seam.rule.bv.ite-merge`. A counter is deterministic,
but it needs `--seam-stats` in both configurations; a counter that B does
not print counts as 0.

## Candidate outcomes

Every candidate gets one outcome, written to the log with its stage:

| outcome | meaning |
|---|---|
| `verifier-rejected` | the MLIR verifier refused it inside `sea-reduce`; the test did not run |
| `verify-failed` | `sea-opt` refused it |
| `sea-vc-error` | `sea-vc` stopped with an error, such as a BMC-form error or a pass postcondition |
| `crashed`, `timeout` | a signal, or the safety timeout |
| `uninteresting`, `interesting` | it ran, and the symptom is absent or present |

Only `interesting` candidates are kept. In `crash` mode, the one
diagnostic that `--match` names counts as the symptom.

## What sea-reduce does

`sea-reduce` is upstream `mlir-reduce` with the sea dialect, every sea and
upstream pass, and reduction patterns for sea programs:

- `--reduction-tree='traversal-mode=0 test=t.sh'`: delete ranges of
  operations, then apply the sea patterns to ranges of operations; one
  test per variant.
- `--sea-reduction-tree='test=t.sh pattern=<p>,...'`: the same search with
  the sea patterns only, without the deletion phase. On a large control
  flow graph, most deletions give programs that the verifier rejects.
- `--opt-reduction-pass='opt-pass=<pipeline> test=t.sh'`: run a pass
  pipeline once; the result is kept only if it is interesting and smaller.

The patterns keep the program valid (unique nondet sites, typed block
arguments, terminators); none of them keeps its meaning:

| pattern | rewrite |
|---|---|
| `memory-bypass` | a store, memset, memmove or memhavoc becomes its incoming memory |
| `drop-assume` | a `sea.assume` is erased |
| `select-branch` | a `cond_br` becomes a `br` to one successor |
| `load-to-nondet` | a load becomes a fresh nondet input |
| `forward-operand` | an operation becomes its first operand of the same type |
| `zero-const` | an integer operation without such an operand becomes `sea.const 0` |
| `erase-dead` | an operation with unused results is erased |

As passes, `sea-reduce-apply{pattern=<name>,... limit=<n>}` applies chosen
patterns (also in `sea-opt`), and `sea-reduce-cleanup` removes dead
operations, unreachable blocks and unused block arguments.

## Limitations

- `select-branch` applied everywhere at once reaches only the branches
  that are still reachable.
- The reduction tree applies one pattern per operation, the one with the
  highest benefit: `select-branch` never tries the other successor.
- The reduced program need not be in BMC form for `sea-vc` without
  `--lower`. Reduce the input of the configuration you run.
- Deleting ranges of operations rarely succeeds in a control flow graph;
  the patterns do most of the work. Each accepted variant costs a full
  test run, so a program with 14,000 operations and a 20-second test takes
  hours.
- `sea-vc` encodes every block, reachable or not, so unreachable blocks
  can carry a symptom.
- There is no pattern that simplifies structs, offsets or lengths, merges
  block arguments, or removes a memory object that is still used.
