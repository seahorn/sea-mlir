# Output formats and exit codes

## Text

`sea-vc` and `sea-run` print `key: value [k=v ...]` lines on stdout, one
fact per line, in a stable order. Diagnostics go to stderr as
`Warning: <message> [kind]` and `Error: <message> [kind]`, where `kind` is
a short stable tag. With `--smt2`, the query goes to stdout and the
statistics to stderr.

The main lines of `sea-vc`:

| line | meaning |
|---|---|
| `tool.version: sea-mlir <x.y.z> (<git describe>)` | the first line of every run |
| `vc.config: <path>` | a profile that was loaded, in order |
| `program: @<name>` | the program checked |
| `vc.ptr-mode: opaque|address` | the pointer model |
| `object: name=obj.<id> order= kind= base= size= align=` | a placed object (with `--seam`); `obj.<id>` is its constant in the query |
| `vc-time: <s>` | query generation, including the SeaM rewriting |
| `vc.ackermann-constraints: <n>` | consistency constraints added for read-only arrays |
| `seam.inb: assumed accesses=<n> conditions=<m>` | the in-bounds assumption was used for $n$ accesses; $m$ of them have a non-constant condition |
| `seam.inb: off (fallback: an access leaves its object)` | the assumption was dropped after a static violation |
| `assume.memset-bound: count=<n> bound=<K>` | $n$ symbolic-length memsets were bounded by $K$ |
| `solve-time: <s>` | the solver's check |
| `result: sat|unsat|unknown|none` | the verdict (`none`: `--vcgen-only`) |
| `solve-timeout: true`, `mem-limit: hit` | why the result is `unknown` |
| `hint: split: rerun with --split-candidates ...` | an `unknown` of a `--seam` run (not a split): where to look next |
| `split.candidates: open-aliases= leaves-candidates= bool-candidates= bool-listed= bool-dropped= bool-values= vc-time=` | `--split-candidates`: the census; `bool-dropped`: Boolean candidates beyond `--split-candidates-max` |
| `split.leaves-candidate: name= leaves= unknown-leaves= open-aliases= direct= own= loc=` | `--split-candidates`: an ite-valued address (a `--split-leaves` selector); `direct`: open questions holding its term itself; `own`: open questions no other candidate is behind |
| `split.bool-candidate: name= target=value\|guard open-aliases= addrs= loc=` | `--split-candidates`: an i1 value or block guard (a `--split` selector) in the ite conditions behind open questions |
| `nondet: site= callee= width= value=` | an input from the model |
| `memread: site= addr= width= value= [obj=&<id>[<off>]]` | a memory cell from the model, with the object and offset it lies in |
| `replay: <status>` | the interpreter's status on the model's inputs, normally `assert-failed` |
| `replay: assert-at: <where>` | where the failing condition is computed ([below](#where-a-counterexample-fails)) |
| `at: <MLIR location>` | the whole MLIR location of the op that ended the replay |
| `replay: assert-op: <where>` | the `sea.assert` that failed |
| `replay: assert-path: <where> <- <where> ...` | up to five branch locations on the way, most recent first |
| `print: <path> = <value>` | a print operation evaluated in the replay ([notation](print.md)) |
| `validate: checked= mismatches= dontcare=` | values compared between the model and the replay |
| `interp.earlier-version-loads: <n>` | loads of an earlier memory version in the replay; `validate` checks their values |
| `mismatch: ...` | one value where they differ (then exit status 2) |
| `seam.rule.<rule>: <n>` | how often a SeaM rule fired (`--seam-stats`) |
| `vc.sink-assumes: <n>` | assumes moved by `--sink-assumes` |
| `vc.fold: assume-false=<n> assert-false=<n>` | the lowering folded an assume or assert of constant false (warnings `fold.assume-false`, `fold.assert-false`; `sea-merge-asserts` also warns `fold.assert-false` for an assert of constant 0, except the only assert right before the halt without a source location: SeaHorn's error marker `assert false; halt`; an assert that becomes constant only in the lowering's canonicalize, such as `icmp ne x, x`, is not reported, though the verdict still shows it); only when one did. The counts are of every program in the file the lowering ran on, not only the selected one |
| `vc.statically-safe: no reachable sea.halt ...` | no path reaches the assertion (every one is blocked, e.g. by a folded `assume` of false): it cannot fail; the solver still answers (`unsat`) |
| `seam.ptrint: assumed loads=<n>` | `--ptr-companion=assume`: split pointer loads that rest on A-PTRINT |
| `seam.ptrint: checked loads=<n>`, then `seam.ptrint: holds` or `seam.ptrint: not discharged` | `--ptr-companion=check`: the loads checked, and the answer (`holds` for `unsat`) |
| `seam.ptrint-violated: <where> addr= [obj=] bits= stored=` | `--ptr-companion=check`, `sat`: a split pointer load in the replay that reads bits other than the pointer last stored there |
| `seam.ptrint-unstored: <where> addr= [obj=] bits=` | `--ptr-companion=check`, `sat`: a split pointer load from a cell no pointer was stored to |
| `seam.ptrint-replay: loads= holds= violated= unstored=` | `--ptr-companion=check`, `sat`: the split pointer loads of the replay; the `sat` is confirmed if one fails |
| `assume.a-inb: drop-fresh-memhavoc=<n>` | `--lower` of an abstract program dropped n memhavocs of fresh objects whose no-op rests on A-INB (also without `--seam` and with `--seam-inb=0`); a separate memory-safety check discharges it |
| `assume.addr0-is-null: vc null-uses=<n>` | `--lower`: placement puts the null object at address 0, so the VC identifies null with address 0 (n: uses of its segment); a fact about the target, not discharged |
| `assume.memhavoc-null-noop: vc sites=<n>` | `--lower`: memhavocs whose pointer may be null, guarded so that the VC runs them only when it is not (`--memhavoc-null=noop`) |
| `assume.memhavoc-null-noop: uses=<n>` | after `replay.original:` (and in `sea-run`): memhavocs of the null pointer the run executed as no-ops, an open semantics decision (`--memhavoc-null`) |
| `assume.addr0-is-null: uses=<n>` | after `replay.original:` (and in `sea-run`): steps of the run that rested on the assumption that address 0 is null; only when there are any |
| `replay.original: status= at= match= nondets= cells= defaulted= unmapped=` | `--replay-original`, `sat`: the input program before any pass, run on the model's inputs; `match=yes` if it fails the same assertion ([below](#replay-on-the-original-program)) |

`sea-vc --agent` lists every line, including all counters.

### Reach

`sea-vc --reach` (`=checks`, the default, or `=blocks`) asks, for each
target of the lowered program, whether an execution reaches the top of it
with every earlier assume holding. It prints no `result:` line: it is not
a safety verdict.

| line | meaning |
|---|---|
| `reach.config: kind=checks|blocks solver=incremental|separate rlimit=<n> splits=<n> model-runs=0|1 order=program|deepest` | the run's settings; `splits`: blocks split so that each target has a block right before it |
| `reach: <id> <status> op=<op> [reason=] [replay=] [mismatches=] [witness=] time=<s> at=<where>` | one target, in program order; `status` is `reachable`, `unreachable` or `unknown` |
| `reach.summary: targets= reachable= unreachable= unknown= queries= time=` | the totals |

- Targets with `checks`: every `sea.assume`, `sea.assert` and
  `sea.unreachable` (where the lowering leaves an assume of constant
  false). With `blocks`: every block, at the location of its first located
  op. Sites the lowering removed (an assume of true, the code after an
  assume of false) are not listed. With `--check-align` or
  `--check-ptr-index`, the one merged assert of the promises is a target
  like any other.
- `site=<k>` (at the end of the line): the reach site of the target, the
  position in program order of the input check it stands for. `--reach`
  tags every `sea.assume` and `sea.assert` of the input with it
  (`sea.reach_site`) before any pass; an input that already has the
  attribute keeps its tags, and a tag two checks share is the error
  `vc.reach-site`. The lowering keeps the tag on the checks it keeps, on
  the `sea.unreachable` of an assume folded to false (the same top of op)
  and on the `sea.assume` `sea-merge-asserts` makes of an assert (reached
  once the assert holds). Checks the lowering makes have none. Two
  lowerings of one input match their targets by site
  (`utils/cover-diff.py`). A check of the constant true (which
  canonicalize erases) also has `cond=true`, and `next=<k>`, the site of
  the next target in its block, if there is one: the check is reached iff
  that target is. With `--reach-keep-sites`, canonicalize replaces such a
  check (tagged) by a reach-site marker, `sea.print "<op>"` with the tag,
  which is a target too: `op=sea.print ... site=<k> of=<op>` (`of` the
  check it stands for; jsonl field `of`). With `--reach-failures`, the
  failure of each tagged assert is a target too: a marker `of=sea.assert.fail`
  with the assert's site and the id `<assert id>.fail`, alone in a block
  reached iff an execution reaches the assert with its condition false
  (`sea-split-assert-failures` before the targets are listed; for a merged
  assert, the landing block of `sea-merge-asserts`).
- The id is the target's `file:line:col`, then the call sites it was
  inlined at, each after `@`; a position in the `.mlir` file itself (an op
  the exporter left without a source position) is written with the file's
  basename, so copies of one program in different directories give the
  same ids; `unlocated#<k>` for a target without a position, and `#<n>` on
  the second and later targets with one id. Ids are stable for one input
  and one set of options.
- `reason`: `cfg` (no path from the entry reaches the target), `rlimit`,
  `timeout`, or the solver's words for an `unknown`.
- `replay`: each `reachable` target's model is run in the interpreter.
  `reached`: the run reaches the target and its values agree with the
  model up to it. `mismatch` or `not-reached` is a bug (exit status 2), so
  is a run that reaches a target the VC answers `unreachable`
  (`vc.reach-inconsistent`; only runs that reached their own target, agree
  with the model and stay inside every object count). `memory-error`: the
  run leaves an object before the target. Under `--seam` every model is
  also run on the input program, whose accesses are checked against their
  objects, as for a counterexample (`vc.seam-inb-spurious`); the answer
  rests on the in-bounds assumption, which that run breaks (exit status 4).
  `inb=undetermined`: that check could not decide (the run reads a nondet
  memory cell).
- `witness`: with `--reach-witness=<dir>`, the run's `nondet:` and
  `memread:` lines, which `sea-run --input-file` replays; its header says
  on which program: the lowered one (with `--lower`; `sea-opt
  --sea-vc-lower` with the same options) or the input program.
- An unreachable check site means unsatisfiable preconditions before it,
  or dead code. The `seam.inb` and `assume.*` lines are the assumptions
  every answer rests on. The reach runs (the replays and the A-INB check)
  use the interpreter's options of the safety replays (`--addr0-is-null`,
  `--memhavoc-null`); after the summary, `assume.addr0-is-null: uses=<n>`
  and `assume.memhavoc-null-noop: uses=<n>` count the steps of all of them
  that rested on those assumptions.

#### Model runs

The replay of a reachable target's model is an execution of the program:
every target it reaches (it is about to execute the target's op) whose
query is still to come is reachable without a query, unless
`--reach-model-runs=false`. Only replays that reach their own target and
agree with the model count; under `--seam`, a target also needs the input
program's run on the model to reach it inside every object, as for the
model's own target.

| line | meaning |
|---|---|
| `reach: <id> reachable op=<op> via=model-run by=<id> [inb=undetermined] [witness=<file>] time=0.000 at=<where>` | a target the model of target `by` reached; `witness`: that model's witness |

- `reach.config` has `model-runs=0|1 order=program|deepest`, and
  `reach.summary` ends with `model-runs=<n>`, the targets decided so.
- The queries go in reverse program order (`--reach-order=deepest`, the
  default; `=program` in order): the model of a deep target passes the
  targets before it.
- The cross-check stays: a clean run that reaches a target the solver
  answered `unreachable` is `vc.reach-inconsistent`.

#### Concrete runs first

With `--reach-corpus` or `--reach-fuzz`, concrete runs decide what they
can before any query, and the solver gets the rest. The corpus files run
first, in order, then the fuzz trials (the mutations of `sea-run --fuzz`);
they run on the program the targets are listed on (after the lowering and
the splits), with the reach replays' interpreter options. The queries of
the rest use a new solver each (`--reach-incremental=false`) unless
`--reach-incremental` is given. A run reaches a
target when it is about to execute the target's op: its prefix up to there
is an execution with every earlier assume holding, which is what the
query asks. What the run does after that (an assume that blocks it, a
failing assert, a memory-safety stop) does not matter.

| line | meaning |
|---|---|
| `reach: <id> reachable op=<op> via=run by=seed:<file>\|trial:<n> [inb=undetermined] [witness=<file>] time=0.000 at=<where>` | a target a run decided; `by`: the first run that reached it, a corpus file or a fuzz trial (1-based, corpus runs counted); `witness`: that corpus file, or with `--reach-witness` the trial's inputs as `<dir>/reach-<k>.txt` |
| `reach.runs: decided=<n> model-run=<n> solver=<n> cfg=<n> unknown=<n> trials=<n> seeds=<n> inb-violated=<n> time=<s>` | after `reach.summary`: the targets the runs decided, model runs decided, the solver decided, the CFG decided (no path), left unknown; the runs made and their time |

- `reach.config` has `corpus=<n> fuzz=<n>` at its end.
- `--reach-query=false`: no query at all; a target no run reached is
  `unknown reason=no-query` (exit status 3), and `reach.config` has
  `query=0`. `utils/cover-diff.py` runs a witness of one lowering so on
  another.
- Under `--seam`, a run that reaches a target also runs on the input
  program, as a model's run does; a run that leaves an object before the
  target, or that leaves one and reaches the target only by a location
  other ops share (a folded assume, a merged assert, a block), so that
  the order is not known, decides nothing (`inb-violated`), and the solver
  answers that target. The same holds for model runs.
- The deciding runs' uses of `addr0-is-null` and of `memhavoc(null)` as a
  no-op are added to the `assume.*: uses=<n>` lines (each run once, the
  whole run).
- The targets the runs decide are not checked against the VC: a run is the
  reference semantics.

#### Block ids

`--reach` also gives every block of the input a stable id before any pass:
`sea.block_ids = ["<n>"]` on its terminator, n its position in the
program. The passes keep them: a block merged into its only predecessor
holds both lists; a split leaves the list on the head and gives the tail
the id `<last>.s<k>` (a piece of the block); a block a pass adds gets
`<last>.<role><k>` (`h` the guarded memhavoc, `m` the memset expansion,
`f` the landing block of a merged assert, `p` / `q` the pass and fail
blocks of an assert's failure target), `x` the exit block of the merged
asserts and `t` R_T's target block; an edge block of
`sea-split-critical-edges` gets `<a>><b>` (the last id of its source, the
first of its target); the ids of an erased block are gone. R_T keeps the
ids, and `sea-vc --smt2` on a program that carries them ends the query
with one comment per block guard, `; sea.block-ids <guard> <id>,<id>...`:
`utils/query-diff.py` builds its Skolem hints from them. A block a path
from the entry reaches without ids (a pass lost them) is the warning
`vc.block-ids-missing`. Without `--reach` (and on a program without ids)
nothing is added.

#### R_T programs

`--reach-programs=<dir>` writes, for each target T, R_T: the program the
targets are listed on (after `--lower`) with every `sea.assert` dropped,
the block of T split right before T and ended by a branch to a new block
`sea.assert false; sea.halt` (T's location; the assert has
`sea.reach_target = "<id>"`), every other `sea.halt` replaced by
`sea.unreachable`, and the blocks no longer reachable from the entry
erased. A run fails R_T's assertion iff it reaches the top of T with every
earlier assume holding: `sea-vc` on R_T answers `sat` iff `--reach` answers
`reachable`, and T's witness fails R_T's assertion in `sea-run`. R_T is in
BMC form when the input is. For a target no path from the entry reaches
(`reason=cfg`), R_T has no assertion and no `sea.halt` left: statically
safe, `unsat`. `sea-vc --reach` on R_T lists its assert with T's id, taken from the
`sea.reach_target` attribute, whatever its location; R_T's other targets
are numbered on R_T (`#n`, `unlocated#k`), and the `sea.unreachable` that
replaced each other `sea.halt` is a target of R_T. Compare R_T programs by
the attribute, not by the other ids. R_T is the program after `--lower` with the run's options:
under `--seam` the lowering keeps memset and memmove of symbolic length,
so run `sea-vc --seam` on it. One `reach.program: <id> op=<op> file=<path>
at=<where> [site=<k>]` line per target, then `reach.programs: <n> dir=<dir>`. `sea-opt
--sea-reach-target="id=<id>"` builds one R_T.

The lines of `sea-run`: `program:`, `nondet:` and `memread:` (with an
`origin`: `input`, `zero` or `random`), `value:` (with `--trace`), the
`print` lines, `status:`, `at:`, `assert-at:`, `assert-op:`,
`assert-path:`, `reached:` (for a `memory-error`), `message:` (for an
error), `deferred:` (with `--defer-memory-errors`, when the run went past a
memory-safety violation) and `steps:`. See
[Deferred memory errors](fuzzing.md#deferred-memory-errors).

### Replay on the original program

`--replay-original` checks the lowering on each counterexample. The replay
validates the model against the lowered program, the one the VC was made
from; a pass that changes what the program does gives a model that the
lowered program confirms. So after a confirmed `sat`, `sea-vc` also runs
the input program as it was before any pass, with the model's inputs:

- a nondet value by its site: the op kind, the site number and the
  callee (a `sea.struct.nondet` is the `sea.nondet` of the same site after
  `sea-scalarize-structs`). The number alone is not enough: a pass may
  reuse the number of an op it dropped;
- a memory cell lazily, when the run reads it: a cell `<object>+<offset>`
  of an abstract memory is the model's cell at the object's placed base
  (its `sea.segment`) plus the offset; a byte of a memory widened to
  aligned words is its byte of the word (little-endian); a `sea.memhavoc`
  the lowering dropped (`sea-drop-fresh-memhavoc`) reads its chain's
  initial `sea.mem.nondet` (through block arguments), as the lowered
  program does.

Inputs the model leaves unconstrained (`defaulted`) and sites or objects
the lowered program no longer has (`unmapped`, for example a load folded
away or an object never accessed) read as 0. The run defers memory-safety
violations, as `sea-run --defer-memory-errors` does.

The check passes (`match=yes`) when the run fails the same assertion:
`assert-failed`, or `memory-error` that reached that assertion. Before
the clone, each `sea.assert` gets an id, the attribute `sea.assert_id`;
`sea-merge-asserts` copies it onto the branches it makes of the assert,
so both runs name the failing assert by its id, also when two asserts
share a location or have none. If a pass rebuilt that branch without
the attribute, the assert is compared by its location, and only if no
other assert has that location.
Otherwise:

- `match=undetermined`, warning `vc.replay-original-undetermined`, exit
  status unchanged: the interpreter cannot run the original program on
  these inputs (`ptr.to_int` before placement, a pointer load from nondet
  cells), or the run loaded an earlier memory version, which the in-place
  interpreter reads right only if the program's Mem-SSA claim holds; or
  the run read an `unmapped` input; or the assert cannot be told apart;
- `match=no` after a memory-safety violation: exit 4
  (`vc.replay-original-memory-error`). Placement gives the program a
  meaning where the abstract program has an error;
- `match=no`: exit 6 (`vc.replay-original-mismatch`). A pass of the
  lowering is wrong, or the program's behavior depends on where placement
  puts its objects (a pointer one past the end of an object compared with
  the next object).

It is off by default. It runs only on `sat`, costs one interpreter run, and
adds the `replay.original:` line. Without a lowering (`--lower` or
`--sink-assumes`) it prints `replay.original: skipped (...)`, and the same
for the check jobs (`--check-align`, `--check-ptr-index`,
`--ptr-companion=check`), whose assertions are not the program's.

### Where a counterexample fails

After `replay: assert-failed` (`sea-vc`) or `status: assert-failed`
(`sea-run`):

- `assert-op` is the failing `sea.assert`. When `sea-merge-asserts` (part
  of `--lower`) merged several asserts into one, it is the original assert
  that failed, and `at:` shows that assert's whole MLIR location.
- `assert-at` is where the failing condition is computed: the op that
  defines it, or, for a chain of `sea.and`, the first false conjunct. For
  a constant or block-argument condition (SeaHorn merges its checks into
  one `assert false` reached by branches), it is the last executed op
  before the assert that has a file location, usually the branch of the
  check that failed. Source locations rank over named locations, which
  rank over positions in the .mlir file. Print operations never count.
- `assert-path` lists up to five branches of the run, most recent first.
  In a program with source locations it lists only source locations: a
  `sea.cond_br` without one is shown at the location of its condition,
  else of the last op before it in its block, and is left out if neither
  has one. A program without source locations keeps its .mlir positions.

### Locations

Each `<where>` above, and `at=` in print lines, has the form

```text
<file>:<line>:<col> [inlined-at=<file:line:col>[,<file:line:col>...]] [name=<name>]
```

The position is first: the op's source location if it has one, else its
position in the input .mlir file, or `-` if there is none. For an op
inlined from a callee (`loc(callsite(<callee> at <caller>))`), the
position is the innermost one, in the callee, and `inlined-at=` lists the
call sites, innermost first and the entry function's call last:
`callsite("f.c":3:5 at callsite("g.c":9:2 at "main.c":20:7))` shows as
`f.c:3:5 inlined-at=g.c:9:2,main.c:20:7`. `name=` is the name of a named
location and is the last field: `loc("claim.x")` on line 154 shows as
`p.mlir:154:3 name=claim.x`, and `loc("claim.x"("a.py":61:4))` as
`a.py:61:4 name=claim.x`. Print and `print.object` lines write the same as
`at=<pos> [inlined-at=...] [at_name=<name>]`.

### Placement files

`sea-vc --lower --place-out=<file>` (`sea-opt` pass option
`sea-place-static{place-out=<file>}`) writes the placement of the
program's objects, one object per line; `#` starts a comment:

```text
# sea-place-static placement of @main: one object per line
name=null kind=null base=0x0 size=0 align=4611686018427387904
name=stack.5 kind=stack base=0xbfffffb0 size=80 align=8
name=global.s_allocator_static kind=global base=0x8048018 size=40 align=8
```

`name` is the object's segment name (`<kind>.<site>` for an allocation,
`nondet.<site>` for a nondeterministic pointer, `global.<name>`,
`func.<name>`, `null`), `kind` its kind, `base` its address, `size` the
units reserved (the allocation size, `max-symbolic-size` for a symbolic
one) and `align` its alignment. A concrete program has no objects to place:
the file has the comment only.

`--place-in=<file>` (`place-in=<file>`) places every object at the base the
file gives it, instead of computing one. The pass still checks the bases
as an allocator, and each violation is an error:

| error | cause |
|---|---|
| `place.in-unplaced` | an object of the program the file does not name (the file's writer did not have it) |
| `place.in-mismatch` | the file's `kind`, `size` or `align` of an object differs from the object's own |
| `place.in-align` | a base that is not a multiple of the object's alignment |
| `place.in-null` | the null object not at 0, or another object at 0 |
| `place.in-width` | an object that does not fit the program's address width |
| `place.in-overlap` | two objects whose address ranges overlap |
| `place.in-format` | a malformed line, an unknown key or kind, an object named twice |
| `place.in-file` | the file cannot be read |

Objects of the file that the program does not have are holes. Two
lowerings of one program that share a file put their common objects at the
same addresses, so their queries agree on `obj.<name>`
(`utils/cover-diff.py`, [Pass validation](../agents/gates.md#pass-validation)).
Either option needs `--lower`, and a module with one program
(`place.file-programs`).

## JSONL

`--format=jsonl` prints every stdout line as one JSON object:

```json
{"kind":"<kind>", "<field>": "<value>", "text":"<the text line or lines>"}
```

`text` is the text rendering, so `jq -r .text` gives back the text output.
Integers are JSON numbers below $2^{53}$ and decimal strings above;
addresses (`base`, `addr`) are numbers too. Field order is stable.
Diagnostics stay on stderr as text. `--agent` prints its guide as text
whatever the format.

```console
$ sea-vc double-bad.mlir --format=jsonl | jq -c 'select(.kind=="nondet" or .kind=="replay")'
{"kind":"nondet","site":0,"callee":"nondet_u32","width":32,"value":10,"text":"nondet: site=0 callee=nondet_u32 width=32 value=10"}
{"kind":"replay","status":"assert-failed","at":"double-bad.mlir:8:9","loc":"loc(\"double-bad.mlir\":9:3)","op":"double-bad.mlir:9:3","text":"replay: assert-failed\nreplay: assert-at: double-bad.mlir:8:9\nat: loc(\"double-bad.mlir\":9:3)\nreplay: assert-op: double-bad.mlir:9:3"}
```

The record kinds of `sea-vc`:

| kind | fields |
|---|---|
| `stat` | `key`, `value`, and one field per `k=v` of the line; `BRUNCH_STAT` lines have `"brunch": true` |
| `line` | `text` only: a line not of the form `key: ...` |
| `program` | `name` |
| `object` | `id`, `symbol` (the query constant, `obj.<id>`), `order`, `objkind`, `base`, `size`, `align` |
| `result` | `value` |
| `nondet` | `site`, `callee`, `width`, `value` |
| `memread` | `site`, `addr`, `width`, `value`, [`obj`, `off`] |
| `replay` | `status`, [`at`, `at_inlined`, `at_name`] (assert-at), [`loc`] (`at:`), [`op`, `op_inlined`, `op_name`] (assert-op), [`path`] (most recent first), [`path_inlined`], [`message`] |
| `validate` | `checked`, `mismatches`, `dontcare` |
| `reach` | `id`, `status`, `op`, [`via`, `by`] (decided by a run), [`reason`], [`replay`], [`inb`], [`mismatches`], [`witness`], `time`, [`at`, `at_inlined`, `at_name`], [`site`, [`of`], [`cond`, `next`]] |
| `reach.program` | `id`, `op`, `file`, [`at`, `at_inlined`, `at_name`], [`site`], [`of`] (`--reach-programs`) |
| `mismatch` | `def`, `type`, `interp`, `model`, [`obj`, `off`] |
| `no-clobber-violated` | `loc` |
| `ptrint` | `status` (`violated` or `unstored`), `at`, `addr`, [`obj`, `off`], `bits`, [`stored`] |
| `ptrint-replay` | `loads`, `holds`, `violated`, `unstored` |
| `replay-original` | `status`, `at`, `match`, `nondets`, `cells`, `defaulted`, `unmapped`, [`earlier_version_loads`], [`expected`, `got`, `message`] (not a match); skipped: `status` `skipped`, `reason` |
| `smt2` | the query (with `--smt2`) |
| `split-part-start` | `part` (`--split*`: the part's own records follow) |
| `split-part` | `part`, `choice`, `result`, `vc_time`, `solve_time`, `dag`, `alias_open`, `alias_open_pairs`, `alias_open_memop`, `rows`, `ack_reads`, `ack_pairs`, `ack_constraints`, `check` |
| `split-verdict` | `result`, `parts`, `unsat`, `sat`, `unknown`, `none`, `hardest_part`, `hardest_solve_time`, `check_unknown`, `check` (`on`\|`off`), `leaf_structural`, [`stopped`] |
| `split-candidate` | `name`, `loc`, `leaves`, `open_aliases` (`--split-auto`) |
| `split-candidates` | `open_aliases`, `leaves_candidates`, `bool_candidates`, `bool_listed`, `bool_dropped`, `bool_values`, `vc_time` (`--split-candidates`) |
| `split-leaves-candidate` | `name`, `loc`, `leaves`, `unknown_leaves`, `open_aliases`, `direct`, `own` |
| `split-bool-candidate` | `name`, `loc`, `target` (`value`\|`guard`), `open_aliases`, `addrs` |
| `hint` | `topic` (`split`), `option` (`--split-candidates`) |
| `print`, `print.mem`, `print.mem.row`, `print.scope`, `print.object`, `print.note`, `print.truncated` | see [Counterexample notation](print.md) and `sea-vc --agent` |

An object has the same id in every record: its print name (`stack.1`,
`heap.3`, `global.g`) in `object.id`, `print.object.id`, and `obj` of
`print`, `print.mem`, `memread`, `mismatch` and `ptrint`. Join records on
it. The `*_inlined` fields are arrays of `file:line:col` strings, the
entry function's call last; `path_inlined` is parallel to `path`, with
`[]` for an entry that was not inlined. Each location field is present
only when it applies. An `i1` printed with `fmt` `auto` has a JSON
boolean as its `value`.

`sea-run` has `program`, `nondet` and `memread` (with `origin`; a cell of
an abstract memory has `obj` and `off` instead of `addr`), `value` (with
`--trace`), `status` (one record for the `status:`, `at:`, `assert-*`,
`reached:`, `message:` and `deferred:` lines, with the location fields of
`replay`; `reached` and `reached_at` for a `memory-error`, `deferred`,
`deferred_kind` and `deferred_at` for a run that went past a violation),
`stat` (for `steps`), `reach` (with `--reach-targets`: `id`, `status`
`reached` or `not-reached`, `op`, [`by`], [`witness`], [`at`,
`at_inlined`, `at_name`]), and the print kinds.

## BRUNCH statistics

`--brunch-stats` adds `BRUNCH_STAT <key> <value>` lines for tools that
collect results into tables: the result, the times, the exit status, the
in-bounds assumption, and with `--seam-stats` every SeaM counter. The
other output is unchanged. The benchmark harness runs with it.

## Exit codes

| status | `sea-vc` |
|---|---|
| 0 | `unsat`, or `sat` confirmed by replay: the replayed run fails an assertion; with `--ptr-companion=check`, a split pointer load fails its A-PTRINT guard (`seam.ptrint-violated` or `seam.ptrint-unstored`) |
| 1 | error: bad input, not in BMC form, unsupported operation, replay impossible |
| 2 | `sat`, but the replay disagrees with the model (a value differs, or the run shows no failure of the property: `vc.replay-no-failure`): a bug, or a load of an earlier memory version whose cell was overwritten (counted as `interp.earlier-version-loads`) |
| 3 | `unknown`, including a timeout or a memory limit |
| 4 | with `--seam`: a `sat` whose replay leaves an object; the in-bounds assumption does not hold, the answer is inconclusive, and the program has a memory-safety error. With `--replay-original`: the original program's run makes a memory-safety violation and does not fail the same assertion |
| 5 | with `--seam-check`: an invalid rewrite; a bug, and the verdict is not to be trusted. It takes precedence over 0, 2, 3 and 4, not over 1 |
| 6 | with `--replay-original`: `sat` confirmed on the lowered program, but the original program on the same inputs does not fail the same assertion: the lowering changed what the program does (a bug in a pass, or a program whose behavior depends on where placement puts its objects) |

With `--reach` the status is not a verdict: 0 when every target is
decided and every reachable one replays to it; 3 when some target is
`unknown`; 4 when a replay stops at a memory-safety violation before its
target; 2 for a replay that disagrees with the model; 1 for an error. Each
takes precedence over the ones before it in this list.

| tool | exit codes |
|---|---|
| `sea-run` | 0: the run ended, with any status but `error` or `memory-error`; 4: `memory-error` (with `--defer-memory-errors`: a memory-safety violation on a run that reached the end or a failing assertion; like `sea-vc`'s 4 (A-INB), plus double free and bad free); 1: error (bad input, a missing input value, an unsupported operation). `--fuzz`: 0 when the trials ran, assertion failures included; 4 when they found a memory-safety finding (`fuzz.memory-safety`, with `--defer-memory-errors`); 1 on a bad option or corpus file, or a failure that does not replay |
| `sea-opt` | 0: ok; 1: a parse, verifier or pass failure |
| `sea-seam-export` | 0: exported; 1: error, nothing on stdout; 3: exported, but some rule firings cannot be expressed |
| `sea-reduce-test test` | 1: interesting; 0: not interesting |
