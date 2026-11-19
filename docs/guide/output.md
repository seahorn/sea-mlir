# Reading the output

`sea-vc` prints one `key: value` line per fact on stdout. Most lines are
statistics. A few carry the verdict and the conditions it holds under.
This page goes through a real run and names the lines to read first. The
complete list is in `sea-vc --agent` and in
[Output formats](../reference/output-formats.md).

## A proof

Start with the verdict, the assumptions and the tool version. This excerpt
comes from the [HPACK decoder proof](../tutorials/firedancer.md):

```console
$ sea-vc firedancer/hpack-huff-proof.mlir --lower --seam --widen-memory=4
tool.version: sea-mlir 0.2.0 (...)
...
assume.a-sep: seam=0
assume.a-null: seam=0
assume.a-nowrap: seam=0
assume.addr0-is-null: vc null-uses=1
seam.premises: assumed accesses=51 conditions=9
...
result: unsat
```

The assertion holds under the reported assumptions. Here no rewrite used a
pointer premise (`seam=0` on each `assume.a-*` line), so this `unsat` does
not depend on them. When one is above 0, the result is conditional: it is
not an independent proof that the accesses respect the premise. To use it,
retain the assumption lines with the verdict and check that they match the
claim your harness is intended to establish.

## What to read first

1. **`result:`** is `unsat` when no violating input exists, `sat` when
   the solver finds one, `unknown` when it cannot decide, or `none` (with
   `--vcgen-only`, which builds the query without solving it). Read it
   together with the exit status
   ([exit codes](../reference/output-formats.md#exit-codes)). For `sat`,
   require a failing replay with no validation mismatches before treating
   it as a counterexample ([Counterexamples](counterexamples.md)).
2. **The assumptions.** `assume.a-sep`, `assume.a-null` and
   `assume.a-nowrap` count the rewrites that rest on each pointer premise
   (no cell accessed through two objects; no access through null or another
   object of size 0; no address wraps). `seam.premises: assumed
   accesses=51 conditions=9` says the premises were on for 51 memory
   accesses; for 9 of them the in-bounds condition, which a bounds check
   would check, was not constant. The other assumption lines are
   `assume.memset-bound` (a memset of symbolic length was bounded),
   `assume.a-inb` (opt-in premise: every access stays
   in its object; only with `--narrow-offsets`),
   `seam.premises: off (fallback: ...)` (the premises were dropped),
   `seam.out-of-object:` (accesses that leave their object on every path
   alternative: the `unsat` holds only if they do not execute) and
   `seam.ptrint` (`--ptr-companion=assume`). An `unsat` reported without
   its assumption lines is incomplete.
3. **Warnings on stderr.** `Warning: ... [kind]` names handling that is
   incomplete or approximate. The verdict is then about the program as the
   tool understood it. One is about the answer itself:
   `vc.ack-own-collision-excused` (`--ack-own`) says the solver's model
   was not a consistent memory (`vc.ackermann-model: ... own-broken=<n>
   own-excused=<n>`), and the `sat` rests on the replayed run alone, which
   failed the assertion and validated. Without such a run the same model
   ends in `vc.ackermann-inconsistent` (status 2).
4. **`tool.version:`** and every `vc.config:` line. Record them with the
   verdict. The defaults are not tuned and can change between versions.

## Detailed output

The same run includes object placement, pointer mode, timings and memory
encoding statistics. These help diagnose a result or compare configurations:

```console
$ sea-vc firedancer/hpack-huff-proof.mlir --lower --seam --widen-memory=4
tool.version: sea-mlir 0.2.0 (...)
vc.typecheck-time: 0.001
program: @main
vc.ptr-mode: opaque
vc.int-to-ptr: 0
vc.ptr-from-int-opaque: 0
object: name=obj.null order=0 kind=null base=0 size=0 align=4611686018427387904
object: name=obj.global.hp_lo order=1 kind=global base=134512640 size=8 align=8
...
object: name=obj.global.hp_in order=6 kind=global base=134512672 size=4096 align=16
object: name=obj.global.hp_out order=7 kind=global base=134516768 size=8192 align=16
object: name=obj.global.huff_decode_table order=8 kind=global base=134524960 size=16448 align=16
vc-time: 0.021
vc.ackermann: on
vc.ackermann-arrays: 2
...
vc.ackermann-constraints: 2
seam.premises: assumed accesses=51 conditions=9
solve-time: 0.424
result: unsat
```

1. **The pointer mode.** `vc.ptr-mode: opaque` refuses integer-to-pointer
   casts; `address` admits them. `vc.int-to-ptr` counts them in the
   program.
2. **The objects.** Every placed object, with its name, kind, address and
   size. Counterexamples refer to memory by these names
   (`&global.hp_in[0]`).
3. **The times.** `vc-time` is the generation of the query, including the
   SeaM rewriting; `solve-time` is the solver. `--timeout` bounds only the
   solver. A blowup in query generation shows as a large `vc-time`, or as
   a run that never prints `solve-time`.

## Counters

The `vc.ackermann-*` lines count the read-only arrays that were replaced
by fresh constants, and the consistency constraints that this added.
`vc.ackermann-constraints` is a direct measure of the memory work left to
the solver.

`--ack-relax=all` (a diagnosis mode) solves without those constraints
first and adds only the ones a model breaks, in rounds on fresh solvers
(`--ack-relax-mode=incremental`: on one solver).
`vc.ack-relax-stop` says how it ended; a sat or unsat verdict is the one the
eager lowering would give (a `consistent` sat is a model of the whole query and
replays as usual). `vc.ack-relax-kept` against `vc.ack-relax-candidates`
shows how many constraints the answer needed, and the
`vc.ack-relax-round:` lines show the solver's effort per round
([Ackermann.md](../design/Ackermann.md#lazy-constraints-ack-relax)).

`--ro-arrays=uf` (with the lowering off) gives the same arrays to the solver
as uninterpreted functions with no constraint, to compare with the solver's
own handling; `vc.ro-arrays` counts them
([Ackermann.md](../design/Ackermann.md#read-only-arrays-as-functions-ro-arraysuf)).

With `--seam-stats`, each SeaM rule reports how often it fired. On a small
NKI gather proof:

```console
$ sea-vc nki/gather-hw-N4D3K2-all-good.mlir --lower --seam --seam-stats
...
vc.ackermann-constraints: 0
seam.premises: assumed accesses=40 conditions=8
...
seam.rule.cmp.diff-base: 26
...
seam.rule.premise.a-sep: 26
...
seam.rule.row.mcopy-hit: 20
seam.rule.row.mcopy-skip: 36
...
result: unsat
```

`cmp.diff-base` decided 26 address comparisons between different objects,
and `premise.a-sep` counts the 26 rewrites that relied on the separation
premise to do so (also printed as `assume.a-sep: seam=26`). A rule that
never fired has no line. Without a `seam.rule.premise.a-*` line, no
rewrite of the run used that premise.

Counters are deterministic; times are not. Compare counters first when two
configurations or two versions behave differently. `sea-reduce-test` can
reduce a program while a counter stays high
([Minimizing a problem](../agents/minimizing.md)).

## Machine-readable output

`--format=jsonl` prints the same lines as JSON objects, one per line, each
with a `kind`, typed fields, and the original `text`:

```console
$ sea-vc double-bad.mlir --format=jsonl | jq -c 'select(.kind=="result" or .kind=="validate")'
{"kind":"result","value":"sat","text":"result: sat"}
{"kind":"validate","checked":6,"mismatches":0,"dontcare":0,"text":"validate: checked=6 mismatches=0 dontcare=0"}
```

Diagnostics stay on stderr as text. The record kinds are listed in
[Output formats](../reference/output-formats.md#jsonl).
