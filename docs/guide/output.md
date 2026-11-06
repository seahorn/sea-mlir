# Reading the output

`sea-vc` prints one `key: value` line per fact on stdout. Most lines are
statistics. A few carry the verdict and the conditions it holds under.
This page goes through a real run and names the lines to read first. The
complete list is in `sea-vc --agent` and in
[Output formats](../reference/output-formats.md).

## A proof

The proof of Firedancer's HPACK Huffman decoder
([Firedancer: HPACK](../tutorials/firedancer.md)), run from a checkout of
the benchmark repository:

```console
$ sea-vc firedancer/hpack-huff-proof.mlir --lower --seam --widen-memory=4
tool.version: sea-mlir 0.1.0 (...)
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
seam.inb: assumed accesses=51 conditions=9
solve-time: 0.424
result: unsat
```

## What to read first

1. **`result:`** is `sat`, `unsat`, `unknown`, or `none` (with
   `--vcgen-only`, which builds the query without solving it). Read it
   together with the exit status
   ([exit codes](../reference/output-formats.md#exit-codes)).
2. **The assumptions.** `seam.inb: assumed accesses=51 conditions=9` says
   that the `unsat` holds for executions in which those 51 memory
   accesses stay inside their objects. For 9 of them the in-bounds
   condition was not constant. The other assumption lines are
   `assume.memset-bound` (a memset of symbolic length was bounded),
   `seam.inb: off (fallback: ...)` (the assumption was dropped), and
   `seam.ptrint` (`--ptr-companion=assume`). An `unsat` reported without
   its assumption lines is incomplete.
3. **Warnings on stderr.** `Warning: ... [kind]` names handling that is
   incomplete or approximate. The verdict is then about the program as the
   tool understood it.
4. **`tool.version:`** and every `vc.config:` line. Record them with the
   verdict. The defaults are not tuned and can change between versions.
5. **The pointer mode.** `vc.ptr-mode: opaque` refuses integer-to-pointer
   casts; `address` admits them. `vc.int-to-ptr` counts them in the
   program.
6. **The objects.** Every placed object, with its name, kind, address and
   size. Counterexamples refer to memory by these names
   (`&global.hp_in[0]`).
7. **The times.** `vc-time` is the generation of the query, including the
   SeaM rewriting; `solve-time` is the solver. `--timeout` bounds only the
   solver. A blowup in query generation shows as a large `vc-time`, or as
   a run that never prints `solve-time`.

## Counters

The `vc.ackermann-*` lines count the read-only arrays that were replaced
by fresh constants, and the consistency constraints that this added.
`vc.ackermann-constraints` is a direct measure of the memory work left to
the solver.

With `--seam-stats`, each SeaM rule reports how often it fired. On a small
NKI gather proof:

```console
$ sea-vc nki/gather-hw-N4D3K2-all-good.mlir --lower --seam --seam-stats
...
vc.ackermann-constraints: 0
seam.inb: assumed accesses=40 conditions=8
...
seam.rule.cmp.diff-base: 26
...
seam.rule.premise.a-inb: 26
...
seam.rule.row.mcopy-hit: 20
seam.rule.row.mcopy-skip: 36
...
result: unsat
```

`cmp.diff-base` decided 26 address comparisons between different objects,
and `premise.a-inb` counts the 26 rewrites that relied on the in-bounds
assumption to do so. A rule that never fired has no line. Without a
`seam.rule.premise.a-inb` line, no rewrite of the run used the in-bounds
assumption.

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
