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
| `mismatch` | `def`, `type`, `interp`, `model`, [`obj`, `off`] |
| `no-clobber-violated` | `loc` |
| `ptrint` | `status` (`violated` or `unstored`), `at`, `addr`, [`obj`, `off`], `bits`, [`stored`] |
| `ptrint-replay` | `loads`, `holds`, `violated`, `unstored` |
| `replay-original` | `status`, `at`, `match`, `nondets`, `cells`, `defaulted`, `unmapped`, [`earlier_version_loads`], [`expected`, `got`, `message`] (not a match); skipped: `status` `skipped`, `reason` |
| `smt2` | the query (with `--smt2`) |
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
`stat` (for `steps`), and the print kinds.

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

| tool | exit codes |
|---|---|
| `sea-run` | 0: the run ended, with any status but `error` or `memory-error`; 4: `memory-error` (with `--defer-memory-errors`: a memory-safety violation on a run that reached the end or a failing assertion; like `sea-vc`'s 4 (A-INB), plus double free and bad free); 1: error (bad input, a missing input value, an unsupported operation). `--fuzz`: 0 when the trials ran, assertion failures included; 4 when they found a memory-safety finding (`fuzz.memory-safety`, with `--defer-memory-errors`); 1 on a bad option or corpus file, or a failure that does not replay |
| `sea-opt` | 0: ok; 1: a parse, verifier or pass failure |
| `sea-seam-export` | 0: exported; 1: error, nothing on stdout; 3: exported, but some rule firings cannot be expressed |
| `sea-reduce-test test` | 1: interesting; 0: not interesting |
