# Logging

Logs show what a tool does while it runs. They are off by default and are
selected at run time, in release builds too. A log point that is off costs
one load and one compare.

Logs are not output. The stdout lines of `sea-vc` and `sea-run`
([Output formats](output-formats.md)) and the statistics do not change when
logs are on. Warnings and errors are diagnostics, not logs: they are always
on.

## Tags and levels

Every log point belongs to a tag: a dotted name, subsystem first
(`vc.store-read`). `--log=help` lists the tags a tool has,
sorted, with the highest level each emits and a one-line description:

```text
$ sea-vc --log=help
log tags (--log=<pattern>[:<level>]; level 1 flow, 2 object, 3 detail; docs/reference/logging.md):
  lower.pass     1  each pass of a pass manager sea-vc runs (--lower, --sink-assumes, --print-refs): ops before and after, time
  vc.phase       1  the start and end of each sea-vc phase (lower, encode, simplify, solver-term, ackermann, solve, replay); the end with time and sizes
  vc.reach       2  each sea-vc --reach query: target, answer, solve and replay time
  vc.solve       1  each solver call of the verdict: backend, result, time, Expr nodes made so far
  vc.store-read  3  a SymStore key read before any write (its initial value made)
  warn.<kind>    2  every occurrence of warning <kind>, not only the first
```

A level says how often an event fires, not how serious it is:

| Level | Name | Fires once per | Count |
|---|---|---|---|
| 1 | flow | phase or pass | tens |
| 2 | object | function, memory object, assert, load | grows with the program |
| 3 | detail | operation | grows with the operations |

The level of a log point is the innermost loop around it. Nothing finer
than one event per operation is a log point at any level: not per rewrite,
per term or per solver step.

Level 1 is cheap enough to turn on for every run, benchmarks included.
Level 2 is meant for one subsystem at a time, level 3 for a debugging
session.

To see what a solver receives, use `--smt2`, not a log.

## The tags

| Tag | Level | Tool | Event |
|---|---|---|---|
| `vc.phase` | 1 | `sea-vc` | the start and end of each phase |
| `lower.pass` | 1 | `sea-vc` | one pass run |
| `vc.solve` | 1 | `sea-vc` | one solver call of the verdict |
| `vc.reach` | 2 | `sea-vc` | `--reach`: one target's query (answer, solve time) and replay (status, time) |
| `vc.store-read` | 3 | `sea-vc` | a memory key read before any write |
| `canon.fold` | 2 | all (canonicalize) | one fold of a sea canonicalizer: `assume-true`, `assume-false`, `assert-true`, `assert-false`, `br-merge`, `load-stored` |
| `fuzz.run` | 1, 2 | `sea-run` | `--fuzz`: start and end (1); each trial that reached new coverage (2) |
| `warn.<kind>` | 2 | all | one occurrence of a warning |

`--log='*:1'` selects the level-1 tags: the flow of a run, one event per
phase, pass or solver call.

### The flow of a sea-vc run

```text
$ sea-vc p.mlir --lower --seam '--log=*:1'
log: vc.phase start phase=lower
log: lower.pass pass=sea-apply-tracking on=sea.program program=@p ops-before=7 ops-after=7 time=0.000
...
log: lower.pass pass=sea-split-critical-edges on=sea.program program=@p ops-before=7 ops-after=7 time=0.000
log: vc.phase end phase=lower time=0.002 ops-before=8 ops-after=8
log: vc.phase start phase=encode run=1
log: vc.phase end phase=encode time=0.000 nodes-made=37 sides=1 guarantees=1
log: vc.phase start phase=simplify
log: vc.phase end phase=simplify time=0.000 nodes-made=40
log: vc.phase start phase=solver-term
log: vc.phase end phase=solver-term time=0.000 nodes-made=40
log: vc.phase start phase=ackermann
log: vc.phase end phase=ackermann time=0.000 on=true arrays=0 pairs=0 nodes-made=40
log: vc.phase start phase=solve
log: vc.solve backend=z3 result=sat time=0.003 nodes-made=44
log: vc.phase end phase=solve time=0.003
log: vc.phase start phase=replay
log: vc.phase end phase=replay time=0.000 status=assert-failed checked=4 mismatches=0 validated=true
```

`vc.phase`: `start phase=<p>` when phase `<p>` starts, `end phase=<p>
time=<seconds>` and the phase's sizes when it ends. The phases, in order:

| Phase | When | Sizes at its end |
|---|---|---|
| `lower` | `--lower` | `ops-before`, `ops-after`: the operations of the module |
| `encode` | always | `nodes-made`; `sides` (side conditions), `guarantees` |
| `simplify` | `--seam` or `--simplify` | `nodes-made` |
| `solver-term` | always | `nodes-made` |
| `ackermann` | always | `on` (the lowering ran), `arrays`, `pairs`, `nodes-made` |
| `solve` | not `--smt2` / `--vcgen-only` | (time only; the `vc.solve` event has the result) |
| `replay` | sat | `status` (of the replayed run), `checked`, `mismatches`, `validated` (no mismatch) |

`nodes-made` is the number of terms made so far in this VC run (the Expr
factory's count, freed terms included): a measure of the work and an upper
bound on the size of the query, read in constant time; it is not the
query's DAG size (`--dag-stats`). A run that starts again (a fallback,
`--ack-stride`'s retry) repeats the phases from `encode`, whose start event
numbers the runs: `run=1`, `run=2`, ... A phase that fails or is killed has a start and no end: the last
start line names the phase a killed run was in. Every event is flushed as
it is written.

`--progress` is the same as `--log=vc.phase`, and `--progress=false` the
same as `--log=vc.phase:0`, at their place among the `--log` entries.

`lower.pass`: one event per pass run of the pass managers `sea-vc` builds
(`--lower`, `--print-refs`, `--sink-assumes`): `pass` (its argument,
`sccp`), `on` (the operation it ran on), `program` (`@<name>` of the
program it ran in, `-` for the module: `sea-vc` lowers every program of the
file, in parallel, and their events interleave), `ops-before`, `ops-after` (the
operations under that operation), `time`. A failed pass has the message
`failed`. The operations are counted only when the tag is on.

`vc.solve`: one event per solver call of the verdict (a run that starts again
calls it again): `backend`, `result` (`sat`, `unsat`, `unknown`), `time`,
`nodes-made`. The auxiliary queries of `--seam-check`, `--eq-audit`,
`--words-check` and `--x-read-table-check` have no event.

## Selecting logs

```text
--log=<pattern>[:<level>],...      (repeatable)
SEA_LOG=<pattern>[:<level>],...    (environment)
```

A pattern is a tag, a prefix ending in `.*` (`vc.*` selects `vc` and every
`vc.<rest>`), or `*`. The level is 1 to 3 and defaults to 1; level 0 turns
the tag off. A tag selected at level L shows the events of levels 1 to L.

`SEA_LOG` is read first, then each `--log` in order. A later entry wins for
every tag it matches, so the command line overrides the environment:

```text
sea-vc p.mlir '--log=*:1,vc.store-read:3'
SEA_LOG='*:1' sea-vc p.mlir --log=vc.store-read:0
```

`SEA_LOG` reaches tools started by scripts and benchmarks without changing
their command lines. An entry that matches no tag is a warning
(`log.unknown-tag`), not an error, so a setting meant for one tool does not
break another. A malformed entry (`a:7`, `a*`) is the warning
`log.bad-spec` and is skipped; the other entries of the list still apply.

Under `--config` ([Profiles](profiles.md)), `--log` entries of the config
files come before those of the command line.

## Repeated warnings

A warning kind prints once; repeats are counted and summarized at exit.
`--log=warn.<kind>:2` prints every occurrence of `<kind>`;
`'--log=warn.*:2'` prints every occurrence of every warning (so does
`'--log=*:2'`).

The old spelling `--log=<kind>` still prints every occurrence of `<kind>`.
It also warns `log.unknown-tag`, since `<kind>` is not a log tag, and names
the new spelling. It will be removed in a later release.

## Events

Each event is one line on stderr, written whole even when passes run in
parallel:

```text
log: <tag> [<msg>] k=v k=v ... [at <loc>]
```

- The keys of an event are fixed, so lines can be matched with `grep`.
- Values are numbers, `true` / `false`, single tokens, or quoted strings
  (`"..."` with `\"`, `\\`, `\n`, `\r`, `\t` escaped and other control
  bytes as `\xNN`). A number that is not finite is `nan`, `inf` or `-inf`
  (a JSON string in jsonl).
- Terms are printed when they have at most 64 nodes, otherwise as
  `#<id> (<n> nodes)`.
- `<loc>` is the MLIR location of the operation, when there is one.
- Lines carry no timestamps, so test output stays deterministic. A
  duration is a field, `time=<seconds>`.

Under `--format=jsonl` (`sea-vc`, `sea-run`) each event is one JSON object,
with the same conventions as the stdout records:

```json
{"log":"<tag>","level":3,"msg":"<msg>","<k>":<v>,...,"loc":"<loc>","text":"<the text line>"}
```

`msg` and `loc` appear only when the event has them; `text` is the text
line. No field of an event is named `log`, `level`, `msg`, `loc` or `text`.

## Log file

`--log-file=<path>` or `SEA_LOG_FILE=<path>` appends the events to a file
instead of stderr, so they stay apart from warnings and progress lines.
`--log-file` overrides the environment. The file is opened at the first
event: a run that logs nothing creates no file.

A `--log-file` that cannot be written is the error `log.file` (exit status
1). A `SEA_LOG_FILE` that cannot be opened is the warning `log.file`, and
the events go to stderr: a setting in the environment does not break a
tool.

## MLIR's options in sea-vc

`sea-vc` accepts MLIR's pass-manager, timing and IR printing options and
applies them to every pass manager it builds (`--lower`, `--print-refs`,
`--sink-assumes`):

| option | effect |
|---|---|
| `--mlir-print-ir-after-all`, `--mlir-print-ir-before-all` | print the program after (before) each pass |
| `--mlir-print-ir-after-change` | only when the pass changed it |
| `--mlir-print-ir-after-failure` | after a pass that fails |
| `--mlir-timing` | a time report per pass, at exit |
| `--mlir-pass-statistics` | the statistics of each pass |
| `--mlir-print-debuginfo` | print locations with the IR |
| `--mlir-print-ir-module-scope` | print the whole module, not only the pass's operation; needs `--mlir-disable-threading` |
| `--mlir-disable-threading` | run passes on one thread |

All of them write to stderr. `sea-opt` has the same options.

## For developers

`include/sea/Support/Log.h`. Declare a tag once, at namespace scope, with
its highest level and a one-line description; emit through `SEA_LOG`, whose
arguments are evaluated only when the tag is on at that level:

```cpp
static sea::log::Tag logStoreRead("vc.store-read", 3,
                                  "a SymStore key read before any write");

SEA_LOG(logStoreRead, 3).msg("read").kv("uses", n).at(op->getLoc());
```

A tag name declared twice stops the tool at startup. Tools call
`sea::log::init()` at the start of `main`, before parsing options, and
`sea::log::setFormat` after it when they have `--format`. Defining
`NSEALOG` compiles every log point out; `--log` remains for warnings.

A new log point needs a use case: a question it answers that the logs
could not answer before. Choose its level by the loop around it.
