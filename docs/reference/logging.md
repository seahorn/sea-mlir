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
  vc.mem         1  a memory heartbeat (--mem-trace): every --mem-trace-interval seconds while Expr nodes are made, and at the end of each phase: memory, live nodes, the top operators, rules and sea ops (docs/agents/blowups.md)
  vc.phase       1  the start and end of each sea-vc phase (lower, encode, simplify, solver-term, ackermann, solve, replay, replay-original); the end with time, memory and sizes; progress inside encode
  vc.reach       2  each sea-vc --reach query: target, answer, solve and replay time; each target a run decided
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
| `vc.phase` | 1 | `sea-vc` | the start and end of each phase; progress inside encode |
| `vc.mem` | 1 | `sea-vc` | a memory heartbeat while terms are made, and the census at the end of each phase ([Diagnosing a VC blowup](../agents/blowups.md)) |
| `lower.pass` | 1 | `sea-vc` | one pass run |
| `lower.box-all` | 1, 2 | `sea-vc`, `sea-opt` | `sea-lower-box-all`: its counters per run, refusals by reason (1); each op lowered, expanded, empty or dead (2) |
| `lower.expand-memmove` | 1, 2 | `sea-vc`, `sea-opt` | `sea-expand-small-memmove`: its counters per run (1); each memmove expanded (bytes) or kept (reason) (2) |
| `lower.assume-fold` | 1, 2 | `sea-vc`, `sea-opt` | `sea-assume-fold`: its counters per run (1); each folded use: `fold` (op, value, by: the fact kind assume / assert / edge) and `fold-int` (op, by) (2) |
| `lower.prune-edges` | 1, 2 | `sea-vc`, `sea-opt` | `sea-prune-edges`: its counters per run (1); each pruned edge, its reason (2) |
| `lower.range-fold` | 1, 2 | `sea-vc`, `sea-opt` | `sea-range-fold`: its counters per run (1); each fold, kind, value, facts used (2) |
| `vc.solve` | 1 | `sea-vc` | one solver call of the verdict |
| `vc.reach` | 2 | `sea-vc` | `--reach`: one target's query (answer, solve time) and replay (status, time); a target a concrete run decided (`via`, `by`) |
| `vc.store-read` | 3 | `sea-vc` | a memory key read before any write |
| `yices.const-array` | 2 | `sea-vc` | `--solver=yices`: an assertion in which a const-array survives the select rewrite (the Yices marshaller refuses it: Yices has no const-array): their count and the assertion |
| `canon.fold` | 2 | all (canonicalize) | one fold of a sea canonicalizer: `assume-true`, `assume-false`, `assert-true`, `assert-false`, `br-merge`, `load-stored` |
| `fuzz.run` | 1, 2 | `sea-run` | `--fuzz`: start and end (1); each trial that reached new coverage (2) |
| `warn.<kind>` | 2 | all | one occurrence of a warning |

`--log='*:1'` selects the level-1 tags: the flow of a run, one event per
phase, pass or solver call.

### The flow of a sea-vc run

```text
$ sea-vc p.mlir --lower --seam '--log=*:1'
log: vc.phase start phase=lower
log: lower.pass pass=sea-lower-seaimp on=sea.program program=@p ops-before=7 ops-after=7 time=0.000
log: lower.pass pass=sea-apply-tracking on=sea.program program=@p ops-before=7 ops-after=7 time=0.000
...
log: lower.pass pass=sea-split-critical-edges on=sea.program program=@p ops-before=7 ops-after=7 time=0.000
log: vc.phase end phase=lower time=0.002 rss-mb=45 vm-mb=88 peak-rss-mb=45 ops-before=8 ops-after=8
log: vc.phase start phase=encode run=1
log: vc.phase end phase=encode time=0.000 rss-mb=45 vm-mb=88 peak-rss-mb=45 nodes-made=37 sides=1 guarantees=1
log: vc.phase start phase=simplify
log: vc.phase end phase=simplify time=0.000 rss-mb=45 vm-mb=88 peak-rss-mb=45 nodes-made=40
log: vc.phase start phase=solver-term
log: vc.phase end phase=solver-term time=0.000 rss-mb=45 vm-mb=88 peak-rss-mb=45 nodes-made=40
log: vc.phase start phase=ackermann
log: vc.phase end phase=ackermann time=0.000 rss-mb=45 vm-mb=88 peak-rss-mb=45 on=true arrays=0 pairs=0 nodes-made=40
log: vc.phase start phase=solve
log: vc.solve backend=z3 result=sat time=0.003 nodes-made=44
log: vc.phase end phase=solve time=0.003 rss-mb=45 vm-mb=88 peak-rss-mb=45
log: vc.phase start phase=replay
log: vc.phase end phase=replay time=0.000 rss-mb=45 vm-mb=88 peak-rss-mb=45 status=assert-failed checked=4 mismatches=0 validated=true
```

`vc.phase`: `start phase=<p>` when phase `<p>` starts, `end phase=<p>
time=<seconds>`, the memory of the process and the phase's sizes when it
ends. The memory, in MB: `rss-mb` (resident now), `vm-mb` (address space
now, what `--mem-limit` bounds) and `peak-rss-mb` (the peak resident size
of the process so far). `rss-mb` and `vm-mb` are read from
`/proc/self/statm` (Linux only; elsewhere they are omitted), `peak-rss-mb`
from `getrusage`. The phases, in order:

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
`--ack-stride`'s retry under `--reach`) repeats the phases from `encode`,
whose start event numbers the runs: `run=1`, `run=2`, ... A phase that
fails or is killed has a start and no end: the last start line names the
phase a killed run was in. Every event is flushed as it is written.

`progress phase=encode`: inside `encode`, one event every
`--progress-interval` seconds (default 10, not tuned; checked before each
operation and after each block, so no event fires while one operation is
being encoded), or with `--progress-interval=0` one after every block.
Fields: `run`, `time` (since the phase started), `blocks` /
`blocks-total` (blocks encoded, of the blocks reachable from the entry),
`ops` / `ops-total` (operations whose encoding started, terminators not
counted), `nodes-made`, the memory as above, and, so far in this run, the
operation that took longest (`slowest-op`, `slowest-op-time`,
`slowest-op-at`) and the one that made the most terms (`biggest-op`,
`biggest-op-nodes`, `biggest-op-at`). An operation's time and terms include
the SeaM rewriting of its loads and stores, which happens while it is
encoded; the joins of a block (its guard, edges and block arguments) are
counted in no operation. `-at` is the operation's source position
(`file:line:col`), else its location name, else `-`. One operation can
take most of the phase (a load read through a long chain of stores): the
`slowest-op` and `biggest-op` fields name it once it is done.

```text
$ sea-vc job.mlir --lower --seam --vcgen-only --progress --progress-interval=5
...
log: vc.phase start phase=encode run=1
log: vc.phase progress phase=encode run=1 time=6.631 blocks=197 blocks-total=611 ops=8863 ops-total=11468 nodes-made=1565526 rss-mb=324 vm-mb=370 peak-rss-mb=324 slowest-op=sea.load slowest-op-time=1.761 slowest-op-at=conn.h:199:25 biggest-op=sea.load biggest-op-nodes=483020 biggest-op-at=conn.h:199:25
```

Under `--mem-limit` the same counters are kept (with no events unless
`vc.phase` is on) for the lines a hit prints
([Output formats](output-formats.md)).

`--progress` is the same as `--log=vc.phase`, and `--progress=false` the
same as `--log=vc.phase:0`, at their place among the `--log` entries.

`vc.mem` (`--mem-trace`, the same as `--log=vc.mem`): what the terms grow
by, while they grow. The Expr factory calls a watch every 4096 terms made;
every `--mem-trace-interval` seconds (default 10, not tuned; a beat also
waits 20 times the time of its own census, which walks the live terms;
`0`: at every check) it writes a `beat` event, and at the end of each
phase a `phase-end` event, with the census of that moment:

```text
log: vc.mem beat phase=encode run=1 time=95.875 rss-mb=2471 vm-mb=2562 peak-rss-mb=2470 nodes-live=5613142 nodes-made=11800583 rw-cache=1781674 rule-calls=1792549 op=sea.load op-at=OUT/fd_h2_tx.c:45:24 op-nodes=120672 top-ops="2880144:overlap,1437851:=,1195324:ite,..." top-rules=359754:decide.memo-hits,...,304168:row.smap-group,... top-sites=1033636:sea.load@OUT/fd_h2_tx.c:44:24,...
```

Fields: `phase` and `run` (the last `vc.phase` start), `time` (since the
start of the run), the memory as for `vc.phase`, `nodes-live` (the
hash-consed terms alive now: the factory's unique table), `nodes-made`,
`rw-cache` (entries of the rewriter's cache, which keep their terms alive)
and `rule-calls` (with `--seam` or `--simplify`), in `encode` the operation
in progress (`op`, `op-at`, `op-nodes`: the terms it made so far), and
three lists of `--mem-top` items (default 8), count first (an operator name
can hold a colon), `-` when empty: `top-ops` (operators by live terms),
`top-rules` (rules by firings since the last beat; a rule that counts
nested rules on a scratch set adds them when it returns), `top-sites` (the
operations of `encode` that made the most terms, the one in progress
included; their loads' and stores' SeaM rewriting is part of them). Events
come only while terms are made: a phase that makes none (the solver's
check) has its `phase-end` event only. With the tag off the factory has
no watch: one compare per term made.

`lower.pass`: one event per pass run of the pass managers `sea-vc` builds
(`--lower`, `--print-refs`, `--sink-assumes`): `pass` (its argument,
`sccp`), `on` (the operation it ran on), `program` (`@<name>` of the
program it ran in, `-` for the module: `sea-vc` lowers every program of the
file, in parallel, and their events interleave), `ops-before`, `ops-after` (the
operations under that operation), `time`. A failed pass has the message
`failed`. The operations are counted only when the tag is on.

`vc.solve`: one event per solver call of the verdict (a run that starts again
calls it again): `backend`, `result` (`sat`, `unsat`, `unknown`), `time`,
`nodes-made`. The auxiliary queries of `--seam-check`, `--eq-audit`
and `--words-check` have no event.

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
  `#<id> (> 64 nodes)` (the size is not counted: at most 65 nodes are walked).
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
| `--mlir-timing` | a time report at exit (stderr): sea-vc's phases (those of `vc.phase`), the lowering's passes nested under `lower` |
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
