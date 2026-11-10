# Command line

All tools take `--help`, `--version` and `--agent`. `--agent` prints a
plain-text guide to the tool's options, output lines and exit codes, and
is the most current reference. This page lists the main options.

| tool | what it does |
|---|---|
| [`sea-vc`](#sea-vc) | verifies a program: builds the query, solves it, replays a counterexample |
| [`sea-run`](#sea-run) | runs a program on given inputs; the reference semantics |
| [`sea-opt`](#sea-opt) | `mlir-opt` with the sea dialect and its passes |
| [`sea-reduce`](#sea-reduce-and-sea-reduce-test) | `mlir-reduce` with the sea dialect and reduction patterns |
| [`sea-reduce-test`](#sea-reduce-and-sea-reduce-test) | interestingness tests and the reduction loop |
| [`sea-seam-export`](#sea-seam-export) | SeaM terms and rule firings as JSON |

## sea-vc

```text
sea-vc <file.mlir> [options]
```

The input is a module with a `sea.program` in BMC form, or a program that
`--lower` brings into that form.

**Encoding**

| option | effect |
|---|---|
| `--lower` | run the lowering first (the passes of `sea-opt --sea-vc-lower`): constant folding, struct scalarization, placement of abstract objects, memory widening, intrinsics, splitting critical edges |
| `--seam` | the SeaM memory rewriter ([Background: SeaM](seam.md)) |
| `--seam-inb=0` | SeaM without the in-bounds assumption |
| `--widen-memory=<k>` | with `--lower`: byte memories whose accesses are all aligned $k$-byte words become word memories |
| `--ptr-mode=auto|opaque|address` | the pointer model; `auto` (default) chooses `address` if the program converts an integer to a pointer |
| `--simplify` | the bit-vector and Boolean normalization of SeaM, without SeaM |
| `--ackermannize=auto|on|off` | replace reads of read-only arrays by fresh constants with consistency constraints |
| `--mem=array|lambda` | the memory encoding; `lambda` is SeaHorn's lambda encoding, z3 only |
| `--config=<name|file>` | read options from a profile ([Profiles](profiles.md)) |
| `--sink-assumes` | experimental: replace the program's assumes by one assume of their conjunction on the path, before the assertion (exact in BMC form); a solver tradeoff, off by default |

**Solving**

| option | effect |
|---|---|
| `--solver=z3|yices|bitwuzla` | the solver; z3 is the default and the supported one |
| `--timeout=<s>` | wall-clock limit of the solver's check; does not bound query generation |
| `--mem-limit=<GB>` | address-space limit of the process (`RLIMIT_AS`) |
| `--z3-param=<key=value>` | a global z3 parameter (repeatable) |
| `--z3-tactic=<tactic>` | z3's default tactic |
| `--yices-param=<key=value>` | a Yices search parameter (repeatable), e.g. `random-seed=N`; Yices times vary widely with the seed, so compare medians over several seeds |
| `--yices-config=<key=value>`, `--yices-logic=<logic>` | the Yices context configuration and logic |
| `--smt2` | print the query as SMT-LIB and exit |
| `--vcgen-only` | build and check the query, print its statistics and `result: none`; no solver |

**Checks**

| option | effect |
|---|---|
| `--seam-check[=<N>]` | check every SeaM rule firing (or $N$ of them) with the solver; exit 5 if one is invalid |
| `--replay-original` | with `--lower` or `--sink-assumes`, on `sat`: also run the input program before any pass on the model's inputs; exit 6 if it does not fail the same assertion ([Replay on the original program](output-formats.md#replay-on-the-original-program)) |
| `--check-align` | with `--lower`: assert every `align` promise instead of the program's assertion |
| `--check-ptr-index` | with `--lower`: assert every no-wrap promise of pointer arithmetic that is used |
| `--ptr-companion=check` | with `--seam`, opaque pointers: check A-PTRINT (a split pointer load reads the pointer last stored at its address) instead of the program's assertions; `unsat` prints `seam.ptrint: holds`, a confirmed `sat` lists the failing loads (`seam.ptrint-violated`, `seam.ptrint-unstored`) and exits 0 |

**Output**

| option | effect |
|---|---|
| `--format=text|jsonl` | text lines (default) or one JSON object per line |
| `--seam-stats` | print SeaM's rule counters |
| `--brunch-stats` | also print `BRUNCH_STAT <key> <value>` lines |
| `--dag-stats` | print the size of the query as a DAG |
| `--progress` | log the start and end of each phase on stderr, or the log file (= `--log=vc.phase`) |
| `--print=lines|tree|off` | how the program's prints are shown in a replay |
| `--print-refs=asis|weak|strong` | convert the program's prints before the lowering |
| `--print-limit=<n>`, `--print-locs` | limit the print lines; add the location of each print |
| `--log=<pattern>[:<level>],...` | select logs; `--log=help` lists the tags; `--log=warn.<kind>:2` prints every occurrence of a warning kind ([Logging](logging.md)) |
| `--log-file=<path>` | append log events to a file instead of stderr |
| `--mlir-print-ir-after-all`, `--mlir-timing`, ... | MLIR's pass-manager options, on the lowering pipeline |

## sea-run

```text
sea-run <file.mlir> [options]
```

| option | effect |
|---|---|
| `--input=<site>=<value>,...` | the value of a nondet site; decimal, negative decimal, or `0x` hex |
| `--input=<site>@<addr>=<value>` | the value of a memory cell of the memory at that site |
| `--missing=error|zero|random` | what a site without `--input` gets (default `error`) |
| `--seed=<n>` | seed for `--missing=random` and `--fuzz` |
| `--trace` | print every computed value |
| `--max-steps=<n>` | step limit |
| `--program=<name>` | the program to run, if the module has several |
| `--format=text|jsonl` | output format |
| `--print`, `--print-refs`, `--print-limit`, `--print-locs` | as for `sea-vc` |
| `--input-file=<file>` | inputs from a corpus file ([Fuzzing](fuzzing.md)) |
| `--fuzz=<n>`, `--fuzz-time=<s>` | fuzz: run trials on generated inputs ([Fuzzing](fuzzing.md)) |
| `--fuzz-sites` | list the input sites and the assumes with their conditions, and exit ([For agents](fuzzing.md#for-agents)) |
| `--fuzz-serve` | run on inputs sent as JSON lines on stdin, one response per line ([Driving the fuzzer from Python](fuzzing.md#driving-the-fuzzer-from-python)) |
| `--defer-memory-errors` | go on past a memory-safety violation; a finding only if the run then reaches the end or a failing assertion (not if an assume blocks it, nor at the step limit or an interpreter limit) ([Deferred memory errors](fuzzing.md#deferred-memory-errors)) |

Abstract programs, with pointers into unplaced objects, run directly:
pointers are an object and an offset, and a null, freed or out-of-bounds
access stops the run with an error, unless `--defer-memory-errors`.

```console
$ sea-run double-bad.mlir --input=0=10
program: @main
nondet: site=0 callee=nondet_u32 width=32 value=10 origin=input
status: assert-failed
at: loc("double-bad.mlir":9:3)
assert-at: double-bad.mlir:8:9
assert-op: double-bad.mlir:9:3
steps: 8
```

The status is `halted`, `assert-failed`, `assume-blocked` (the run is
infeasible), `step-limit`, or `error`.

## sea-opt

```text
sea-opt <file.mlir> [passes] [options]
```

`sea-opt` parses, verifies and prints a program, and runs passes on it.
The verifier checks what reading misses: types of block arguments, unique
nondet sites, cast widths, terminators.

| command | effect |
|---|---|
| `sea-opt p.mlir` | parse, verify, print |
| `sea-opt p.mlir --sea-vc-lower` | the lowering that `sea-vc --lower` runs |
| `sea-opt p.mlir --sccp` | fold constants |
| `sea-opt p.mlir --sea-place-static` | place abstract objects at addresses |
| `sea-opt p.mlir --mlir-print-debuginfo` | keep locations, and with them print names, between tools |
| `sea-opt --help` | every pass and option |

Each pass states its contract and checks its postcondition. A
`<pass>.postcondition` error is a bug in the pass.

## sea-reduce and sea-reduce-test

`sea-reduce` is upstream `mlir-reduce` with the sea dialect, its passes,
and reduction patterns for sea programs. `sea-reduce-test` writes the
interestingness test for a symptom and runs the reduction loop:

```text
sea-reduce-test reduce --mode <mode> <mode options> <in.mlir> -o <out.mlir> [--log <file>]
```

Modes, outcomes and patterns: [The reducer](reducer.md).

## sea-seam-export

```text
sea-seam-export <file.mlir> [--lane=<lane>] [--firings] [--address-bits=<N>]
```

Prints the SeaM terms of one straight-line, placed `sea.program` as one
JSON document, for the Lean conformance checks. Lanes and format:
[SeaM export](seam-export.md).
