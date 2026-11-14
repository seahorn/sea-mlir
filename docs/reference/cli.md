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
| `--lower-opt=false` | with `--lower`: no cse and no canonicalize (every op stays) |
| `--print-lowered=<file>` | write the program the VC encodes (after the lowering and every other pass of sea-vc) to this file, with its locations, named as the VC names it: the names of `--split` / `--split-leaves` selectors and of the `split.*` and other report lines (a nondet result is `nd.<site>` in the VC). The run goes on (`--vcgen-only`: no solver); the file is input to sea-vc without `--lower`, with the same names |
| `--place-out=<file>` | with `--lower`: write the placement of the program's objects to a file, one object per line ([Placement files](output-formats.md#placement-files)) |
| `--place-in=<file>` | with `--lower`: place the objects at the bases a `--place-out` file gives, checked as placement does; an object the file does not name is an error (`place.in-unplaced`). Two lowerings that share a file put their common objects at the same addresses |
| `--lower-disable=<pass>,...` | with `--lower`: leave these optimization passes out of the lowering: `sccp`, `cse`, `canonicalize`. The lowering without one against the lowering with it validates the pass (`utils/cover-diff.py`, [Pass validation](../agents/gates.md#pass-validation)). `sea-drop-fresh-memhavoc` is refused: it is a semantics decision (a memhavoc of a fresh object is no tracked modification; without it, one after `sea_tracking_on` is) |
| `--seam` | the SeaM memory rewriter ([Background: SeaM](seam.md)) |
| `--seam-inb=0` | SeaM without the in-bounds assumption |
| `--widen-memory=<k>` | with `--lower`: byte memories whose accesses are all aligned $k$-byte words become word memories |
| `--ptr-mode=auto|opaque|address` | the pointer model; `auto` (default) chooses `address` if the program converts an integer to a pointer |
| `--simplify` | the bit-vector and Boolean normalization of SeaM, without SeaM |
| `--ackermannize=auto|on|off` | replace reads of read-only arrays by fresh constants with consistency constraints |
| `--mem=array|lambda` | the memory encoding; `lambda` is SeaHorn's lambda encoding, z3 only |
| `--config=<name|file>` | read options from a profile ([Profiles](profiles.md)) |
| `--sink-assumes` | experimental: replace the program's assumes by one assume of their conjunction on the path, before the assertion (exact in BMC form); a solver tradeoff, off by default |
| `--box-all-expand=<K>` | with `--lower`: a `sea.box.all` whose upper bounds are constants (the op folders reduce them to one before sccp: `sea.const`, flag-free arithmetic on constants; not block arguments or `nsw` / `nuw` arithmetic) and whose trip count is at most $K$ ($K \le 1024$) is expanded (the body at every index, and-reduced; exact); others get one nondet index; the same polarity rule for both; 0: never; prints `vc.box-all` `expanded=`; default 16 (untuned, [sea.box.all](../design/BoxAll.md#expansion-of-small-boxes)) |
| `--expand-memmove=<K>` | with `--lower`: a memmove of a constant length $\le K$ index units (bytes in byte memories; $K \le 1024$) becomes loads of every source cell, then stores in address order (`sea-expand-small-memmove`, exact; kept with `--ptr-companion`; a fat-pointer companion memmove only with its data memmove; [Short memmoves as copies](../design/SeaM-memops.md#short-memmoves-as-copies-expand-memmovek)); for `--seam` reads whose range test stays open; prints `vc.expand-memmove`; pays for short copies, loses for long ones: 0 (off) by default (untuned) |
| `--range-fold=off\|on\|check` | with `--lower`: fold comparisons, i1 ops and select conditions that integer ranges settle under dominating assume bounds (`sea-range-fold`, [Range folding](../design/RangeFold.md)); `check` asserts every fold instead (the paranoid job); off by default (untuned) |
| `--vc-join-cases` | experimental prototype, off by default: build each ite join (block arguments, values and memories, and the pointer companions of `--ptr-companion`) as a `cases` term, expanded at once to the same ite chain: the query does not change. Prints `cases.<counter>: <total> scalar=<n> memory=<n> companion=<n>` lines (stderr with `--smt2`): the joins, their arity, the repeated values among the joined values, the ones adjacent (a sequential merge folds them; `ite-folded-pairs` are those the ite chain already folds) and the ones only a reordering could fold (`nonadjacent-redundant`). Needs `--vc-join=ite`. The join order is the predecessor use-list order, so the default (last) value is the edge from the branch created first |
| `--vc-cases-reorder` | experimental, off by default, needs `--vc-join-cases`; changes the query: a pair of a join whose value is an earlier pair's value merges into that pair, with the condition `c_i | (c_j & !c_(i+1) & ... & !c_(j-1))` (moving a pair ahead conjoins the negations of the conditions it skips; first-match semantics, no exclusivity of the edges used). Adds the `cases.reorder-*` counters: `reorder-joins` (joins changed), `reorder-merged-pairs` (of which `reorder-merged-adjacent` skip no pair), `reorder-merge-negations`, `reorder-pairs` (pairs left) |
| `--vc-cases-drop-default` | experimental, off by default, needs `--vc-join-cases`; changes the query: a pair of a join whose value is the default is dropped, and the negation of its condition is conjoined into the later pairs (`cases.reorder-dropped-default`, `cases.reorder-drop-negations`). Combines with `--vc-cases-reorder` |

**Solving**

| option | effect |
|---|---|
| `--solver=z3|yices|bitwuzla` | the solver; z3 is the default and the supported one |
| `--timeout=<s>` | wall-clock limit of the solver's check; does not bound query generation |
| `--mem-limit=<GB>` | address-space limit of the process (`RLIMIT_AS`); a hit prints where it was (`mem-limit-phase:`, in encode also `mem-limit-progress:`, `mem-limit-op:`; [Output formats](output-formats.md)) |
| `--z3-param=<key=value>` | a global z3 parameter (repeatable) |
| `--z3-tactic=<tactic>` | z3's default tactic; `sls-smt` is z3's stochastic local search (see [Local search in the solver](fuzzing.md#local-search-in-the-solver)) |
| `--yices-param=<key=value>` | a Yices search parameter (repeatable), e.g. `random-seed=N`; Yices times vary widely with the seed, so compare medians over several seeds |
| `--yices-config=<key=value>`, `--yices-logic=<logic>` | the Yices context configuration and logic |
| `--smt2` | print the query as SMT-LIB and exit |
| `--vcgen-only` | build and check the query, print its statistics and `result: none`; no solver of the query (`--eq-audit` still runs its own checks; `--vcgen-only` skips `--seam-check` and `--x-read-table-check`: warning `vc.vcgen-only-check-skipped`) |

**Checks**

| option | effect |
|---|---|
| `--seam-check[=<N>]` | check every SeaM rule firing (or $N$ of them) with the solver; exit 5 if one is invalid |
| `--words-check-rlimit=<n>`, `--eq-audit-rlimit=<n>`, `--seam-check-rlimit=<n>` | z3 resource limit per auxiliary check; deterministic, unlike `--words-check-timeout`, `--eq-audit-timeout` and `--seam-check-timeout` (seconds, which stay). Default 0: unset. The default is untuned: it stays unset until a corpus sweep sets one. A check that exhausts it is `unknown`: `reason=rlimit` on `words.check` and `seam-check-unknown`, `unknown-rlimit=<items>` on `eq-audit`. Only z3 has an rlimit; with another backend `--seam-check-rlimit` warns (`vc.limit-unsupported`) and the checks run without it |
| `--memhavoc-null=noop\|error` | `sea.memhavoc` of the null pointer: a no-op (default; in the lowering, `sea-guard-null-memhavoc` guards every memhavoc whose pointer may be null, `assume.memhavoc-null-noop: vc sites=<n>`, and in the replays, counted and warned) or a memory-safety error (no guard: the VC havocs from address 0) |
| `--addr0-is-null=false` | the replays only, without the named assumption that address 0 is null (default on). The VC identifies null with address 0 either way (placement; `assume.addr0-is-null: vc`), and sea-vc warns `vc.addr0-is-null-off` |
| `--replay-original` | with `--lower` or `--sink-assumes`, on `sat`: also run the input program before any pass on the model's inputs; exit 6 if it does not fail the same assertion ([Replay on the original program](output-formats.md#replay-on-the-original-program)) |
| `--check-align` | with `--lower`: assert every `align` promise instead of the program's assertion |
| `--check-ptr-index` | with `--lower`: assert every no-wrap promise of pointer arithmetic that is used |
| `--reach[=checks|blocks]` | instead of the safety query: for each check site (`sea.assume`, `sea.assert`, `sea.unreachable`) or each block of the lowered program, whether an execution reaches it with every earlier assume holding; one `reach:` line per target ([Output formats](output-formats.md#reach)) |
| `--reach-rlimit=<n>` | with `--reach`: z3's resource limit per query (deterministic); a query over it is `unknown reason=rlimit` |
| `--reach-witness=<dir>` | with `--reach`: write the inputs of each reachable target's run to `<dir>/reach-<k>.txt`, a `sea-run --input-file` corpus file |
| `--reach-corpus=<dir|file>,...` | with `--reach`: before any query, run these corpus files (an earlier `--reach-witness` directory, `sea-run --fuzz-out` files) on the program the targets are on; a target a run reaches is `reachable via=run`, not queried ([Concrete runs first](output-formats.md#concrete-runs-first)) |
| `--reach-fuzz=<n>`, `--reach-seed=<n>` | with `--reach`: after the corpus runs, `n` fuzz trials (seed `--reach-seed`, default 0) before any query; targets they reach are not queried |
| `--reach-programs=<dir>` | with `--reach`: write R_T, the program with every assert dropped and an `assert(false)` at T, for each target T to `<dir>/reach-<k>.mlir`, and exit; `sea-vc` on R_T answers `sat` iff T is reachable ([R_T programs](output-formats.md#r_t-programs)) |
| `--reach-incremental=false\|true` | with `--reach`: `false`, a new solver per target, with the target's guard asserted; `true`, one solver that checks each guard as an assumption. z3 answers an assumption with its incremental core, which finds a reachable target faster and can prove an unreachable one much more slowly. Default `true`, and `false` with `--reach-corpus` or `--reach-fuzz` (the queries left are mostly unreachable targets) |
| `--reach-model-runs=false` | with `--reach`: query every target. By default the replay of a reachable target's model decides every target it reaches whose query is still to come (`via=model-run`) |
| `--reach-failures` | with `--reach`: also, for each tagged assert, its failure as a target (`op=sea.print of=sea.assert.fail`, id `<assert id>.fail`): reached iff an execution reaches the assert with its condition false |
| `--reach-keep-sites` | with `--reach`: the lowering's canonicalize replaces a tagged assume or assert of the constant true by a reach-site marker (`sea.print "<op>"` with the `sea.reach_site`; prints are inert in the VC) instead of erasing it; the marker is a target (`op=sea.print of=<op>`) |
| `--reach-query=false` | with `--reach` and `--reach-corpus` or `--reach-fuzz`: no query; the runs decide what they reach, every other target is `unknown reason=no-query` |
| `--reach-order=program\|deepest` | with `--reach`: the order of the queries; `deepest` (default), reverse program order, so that a model's run passes the targets before its own |
| `--ptr-companion=check` | with `--seam`, opaque pointers: check A-PTRINT (a split pointer load reads the pointer last stored at its address) instead of the program's assertions; `unsat` prints `seam.ptrint: holds`, a confirmed `sat` lists the failing loads (`seam.ptrint-violated`, `seam.ptrint-unstored`) and exits 0 |

**Case splitting** (Shannon cofactoring of the VC; a part per case, each
answered and counted)

| option | effect |
|---|---|
| `--split=<sel,...>` | split on an i1 value (1, 0) or a block guard (true, false) |
| `--split-leaves=<sel,...>` | split on an address or integer (iN, N > 1) value whose term is an ite tree: one part per distinct leaf. `@<file>:<line>[:<col>]` prefers address op results, then address block arguments, then integer op results, then integer block arguments (`--split`: i1 op results, then block arguments); only the first non-empty class may be ambiguous, `:<col>` picks within it |
| `--split-auto` | with `--seam`: pick the ite-valued address behind the most open alias questions, split on its leaves |
| `--split-candidates` | with `--seam`: list the targets of `--split-leaves` (every ite-valued address) and of `--split` (i1 values and block guards in the ite conditions behind open alias questions or in the indices of array reads, the reads the Ackermann lowering pairs), ranked by the open questions, and when there are none by those reads; also the integer and i1 values and guards by the multiplications and divisions they control (`split.arith-candidate`, a heuristic); no split, no solver (`result: none`) |
| `--split-candidates-max=<n>` | the most `split.bool-candidate` and `split.arith-candidate` lines (default 20; 0: all); the dropped ones are counted |
| `--split-check` | prove each split level exhaustive and exact with z3 (default on; `--split-check-rlimit=<n>`) |
| `--split-max-parts=<n>`, `--split-max-paths=<n>` | the cap on the parts (default 64, untuned; more is an error) and on the paths of an ite tree |
| `--ite-stats` | report the memories, addresses and integers feeding mul / div / rem whose terms are ite trees, per source location (leaves, conditions, guards among their atoms), and the loads at non-ground addresses by kind and location (`ite.*`; the first encoding, before any solver; also with `--vcgen-only`; stderr under `--smt2`; not with a split) |
| `--ite-stats-max=<n>` | the most `ite.site` lines per kind and `ite.load-site` lines (default 40, untuned; 0: all) |
| `--split-stop-on-sat` | stop at the first sat part |
| `--split-smt2=<prefix>` | write each part's query to `<prefix>.part<N>.smt2` (with `--vcgen-only`: no solving) |

A selector is a VC name of the lowered program (`%x`, `nd.<site>`,
`^bb3`, as `--print-lowered=<file>` writes it) or `@<file>:<line>`. An
ambiguous `@<file>:<line>` lists every match (name, op, type, column); one
that matches nothing lists the results of other types on the line and the
nearest lines with results of the kind. The verdict is sat if a part is sat (replayed
as usual), unsat if every part is, else unknown; `split.part:` lines give
each part's counters, answer and time. Not knowing the names, start with
`--split-candidates`; a `--seam` run that ends `unknown` with open alias
questions, or with none but Ackermann constraints, prints a `hint:
split: open-aliases=<n>; ...` line that points to it.

**Output**

| option | effect |
|---|---|
| `--format=text|jsonl` | text lines (default) or one JSON object per line |
| `--seam-stats` | print SeaM's rule counters |
| `--brunch-stats` | also print `BRUNCH_STAT <key> <value>` lines |
| `--dag-stats` | print the size of the query as a DAG |
| `--arith-stats` | print the multiplications, divisions and remainders of the query the solver gets, by operator, width and operand kind (numeral, ite of numerals, ite with a symbolic leaf, two non-constant terms), and per source location of the sea op that made them (`arith.*` lines; works with `--vcgen-only`) |
| `--progress` | log the start and end of each phase on stderr, or the log file (= `--log=vc.phase`); the end with the memory; progress lines inside `encode` |
| `--progress-interval=<s>` | the `vc.phase progress` lines inside `encode`: one every `s` seconds (default 10, not tuned); `0`: one after every block ([Logging](logging.md)) |
| `--print=lines|tree|off` | how the program's prints are shown in a replay |
| `--print-refs=asis|weak|strong` | convert the program's prints before the lowering |
| `--print-limit=<n>`, `--print-locs` | limit the print lines; add the location of each print |
| `--log=<pattern>[:<level>],...` | select logs; `--log=help` lists the tags; `--log=warn.<kind>:2` prints every occurrence of a warning kind ([Logging](logging.md)) |
| `--log-file=<path>` | append log events to a file instead of stderr |
| `--mlir-print-ir-after-all`, `--mlir-timing`, ... | MLIR's pass-manager options, on the lowering pipeline; `--mlir-timing` also times sea-vc's phases ([Logging](logging.md)) |

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
| `--fuzz-ranges=false`, `--fuzz-range-share=<p>` | with `--fuzz`: no range draws, or the probability `p` that a trial uses them (default on, `p` = 0.5: in a range trial a value site that an assume bounds gets values from its range, biased to the ends; the other trials use the generator without ranges) ([Ranges](fuzzing.md#ranges)) |
| `--fuzz-dict=false`, `--fuzz-dict-share=<p>` | with `--fuzz`: no dictionary values, or the probability `p` that a fresh value and that a mutation is one (default: 1/2 of fresh values, 1/5 of mutations) ([Trials](fuzzing.md#trials)) |
| `--fuzz-progress=<s>`, `--fuzz-coverage-out=<file>` | with `--fuzz`: a `fuzz.progress` line (cost and coverage so far) every `s` seconds; the covered block, edge, assume ids and path hashes written to `file` at the end ([Progress](fuzzing.md#progress)) |
| `--fuzz-cmp`, `--fuzz-gates`, `--fuzz-reachable=<file>` | with `--fuzz`: model coverage, measures only (`--fuzz-cmp` is on by default, `--fuzz-cmp=false` turns it off): comparison buckets and branch distances; one `fuzz.gate` line per assume; block coverage of the reachable blocks of a `sea-vc --reach=blocks` output on the same file ([Model coverage](fuzzing.md#model-coverage)) |
| `--fuzz-keep=<reason>,...`, `--fuzz-corpus-max=<n>` | with `--fuzz`: why a trial joins the corpus, `blocks` (default), `cmp`, `frontier`, `all`; at most `n` entries, kept trials past it dropped and counted ([Corpus feedback](fuzzing.md#corpus-feedback)) |
| `--fuzz-sync=<dir>,...`, `--fuzz-sync-period=<s>` | with `--fuzz`: every `s` seconds (default 5) run the new-coverage files of other processes' `--fuzz-out` directories as trials; not reproducible ([Corpus sync](fuzzing.md#corpus-sync)) |
| `--fuzz-strategy=corpus\|mcmc`, `--fuzz-mcmc-beta=<b>`, `--fuzz-mcmc-restart=<n>`, `--fuzz-mcmc-share=<p>` | with `--fuzz`: how a trial gets its input: a mutated corpus entry (default) or a Metropolis-style chain led by a cost (approach level and branch distance); the chain's acceptance (default `b` = 1, not tuned), its restart after `n` moves without a new best (default 1000), the share `p` of trials that are chain moves (default 1) ([MCMC strategy](fuzzing.md#mcmc-strategy)) |
| `--fuzz-sites` | list the input sites, the assumes with their conditions and the ranges, and exit ([For agents](fuzzing.md#for-agents)) |
| `--fuzz-serve` | run on inputs sent as JSON lines on stdin, one response per line ([Driving the fuzzer from Python](fuzzing.md#driving-the-fuzzer-from-python)) |
| `--memhavoc-null=noop\|error` | `sea.memhavoc` of the null pointer: a no-op (default; an open semantics decision, counted as `assume.memhavoc-null-noop: uses=<n>` and warned) or a memory-safety error |
| `--addr0-is-null=false` | turn off the named assumption that address 0 is the null pointer (default on): zero bytes loaded as a pointer, a null pointer's bytes loaded as an integer, and `ptr.to_int` of null then stop the run; on, each such step counts (`assume.addr0-is-null: uses=<n>`) ([Memory-Step2](../design/Memory-Step2.md)) |
| `--reach-targets[=checks|blocks]` | report which reach targets (those of `sea-vc --reach`, with its ids) the run, or the `--fuzz` trials, reached: one `reach:` line per target ([Reach coverage](fuzzing.md#reach-coverage)) |
| `--reach-witness=<dir>` | with `--fuzz --reach-targets`: the inputs of the first trial that reached each target, as `<dir>/reach-<k>.txt` |
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
| `sea-opt p.mlir --sea-place-static` | place abstract objects at addresses (`--sea-place-static="place-out=<file>"` writes the placement, `place-in=<file>` takes it from a file) |
| `sea-opt p.mlir --sea-reach-target="id=<id>"` | R_T of a reach target (an id of `sea-vc --reach`; `kind=blocks` for a block target): every assert dropped, an `assert(false)` at the top of the target, every other `sea.halt` a `sea.unreachable` |
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
