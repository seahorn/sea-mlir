# Counterexamples

When the solver answers `sat`, `sea-vc` does not report the solver's model.
It turns the model into inputs, runs the program in the interpreter, and
reports that run.

## Replay

The model gives a value to every input site and to every cell of an input
memory that the run reads. The interpreter runs the program on
those values and compares every value it computes with the model. There
are three outcomes.

- The run fails the assertion and every compared value agrees:
  `replay: assert-failed` and `validate: ... mismatches=0`. Exit status 0.
  This is a counterexample.
- The run and the model disagree. Exit status 2. Either the query or the
  interpreter is wrong; this is a bug in sea-mlir.
- With `--seam`, the run leaves an object. Exit status 4, with a
  `seam.inb-violation:` line. The model relies on an out-of-bounds access,
  so the in-bounds assumption does not hold for it. The program has a
  memory-safety error, and the answer about the assertion is
  inconclusive.

`validate: checked=<n> mismatches=<m> dontcare=<d>` counts the compared
values. `dontcare` counts values the model leaves unconstrained; they are
not compared, and an unconstrained input replays as 0.

## Reading a counterexample

A mutant of the HPACK Huffman decoder writes one extra output byte per
input byte ([Firedancer: HPACK](../tutorials/firedancer.md)):

```console
$ sea-vc firedancer/hpack-huff-mut-extra.mlir --lower --seam --widen-memory=4
...
result: sat
...
memread: site=5 addr=134512672 width=8 value=192 obj=&global.hp_in[0]
memread: site=7 addr=134541392 width=8 value=0 obj=&global.huff_decode_table[16432]
...
nondet: site=9 callee=nd_ulong width=64 value=1
nondet: site=10 callee=nd_int width=32 value=0
nondet: site=11 callee=nd_ushort width=16 value=3840
...
replay: assert-failed
replay: assert-at: OUT/nghttp2_hd_huffman.c:59:7
at: loc("firedancer/hpack-huff-mut-extra.mlir":321:3)
replay: assert-op: firedancer/hpack-huff-mut-extra.mlir:321:3
replay: assert-path: OUT/nghttp2_hd_huffman.c:59:7 <- OUT/nghttp2_hd_huffman.c:58:9 <- ...
validate: checked=157 mismatches=0 dontcare=1
```

- `nondet:` lines are the inputs, by site, with the name of the function
  that produced them in C: the input length $n = 1$
  (`nd_ulong`), a call that is not the final one (`nd_int` = 0), and the
  decoder state `0x0f00` (3840).
- `memread:` lines are memory cells the model chose, with the object and
  offset they lie in: the input byte `0xc0` (192) at `hp_in[0]`, and
  entries of the decode table.
- `assert-at` is the C source location of the failed check, taken from the
  debug locations the exporter kept. When the assertion's condition is
  computed, `assert-at` is the op that computes it, or the first false
  conjunct of a chain of `sea.and`. SeaHorn instead merges all checks of a
  program into one `assert false` reached by branches; then `assert-at`
  is the last located op the run executed before it, the branch of the
  check that failed.
- `assert-op` is the `sea.assert` in the MLIR file, and `at` is its whole
  MLIR location. When `--lower` merged several asserts into one, both name
  the original assert that failed.
- `assert-path` lists up to five source locations of the branches the run
  took on its way to the failure, most recent first. A branch without a
  source location is shown at its condition's location, or left out.

Programs without C debug locations, such as the traced NKI kernels, get
`assert-at` positions in the MLIR file. A location that carries a name
adds it (`name=<name>`), and an op inlined from another function adds its
call sites (`inlined-at=<file:line:col>,...`); see
[Locations](../reference/output-formats.md#locations).

## Prints

A program can say what to show in a counterexample with print operations:
`sea.print` for a value, `sea.print.mem` for memory, and
`sea.print.scope` / `sea.print.end` to group them. The query does not
encode them: it is byte for byte the same with and without them. They are
evaluated during the replay.

The program of the regression test `test/VC/print/replay.mlir` allocates
two list nodes and links them. Its prints:

```mlir
sea.print.scope "a"
sea.print.mem "val" = "mem"["a"] : i64 {fmt = "s"}
sea.print.mem "next" = "mem"["a"] : !sea.ptr<64> {offset = 8}
sea.print.end "a"
sea.print "s" %s : !sea.struct<i64, !sea.ptr<64>> {names = ["val", "next"]}
sea.print "end" = "end" : !sea.ptr<64>
sea.print "b" = "b" : !sea.ptr<64>
sea.print.mem "bval" = "mem"["b"] : i64
sea.print.mem "dump" = "mem"["a"] : i8 {fmt = "hexdump", count = 32}
```

With the program saved as `list.mlir`:

```console
$ sea-vc list.mlir --lower
...
result: sat
nondet: site=4 callee=nondet_u64 width=64 value=100
replay: assert-failed
...
print.object: heap.2 kind=heap size=16 label=node a at=list.mlir:23:8 at_name=a base=0x8048000
print: a.val = 100 : si64
print.object: heap.3 kind=heap size=16 label=node b at=list.mlir:24:8 at_name=b base=0x8048010
print: a.next = &heap.3[0]
print: s.val = 100 : ui64
print: s.next = &heap.2[8]
print: end = <dangling: end>
print: b = &heap.3[0]
print: bval = 0 : ui64 (default)
print: dump = heap.2[0..32]
print: dump +00: 64 00 00 00 00 00 00 00 10 80 04 08 00 00 00 00 |d...............|
print: dump +10: .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. |................|
validate: checked=60 mismatches=0 dontcare=0
```

- Pointers print as an object and a byte offset (`&heap.3[0]`), not as an
  address. Each object is introduced once by a `print.object:` line, with
  its allocation site (`at=`, and `at_name=` for a named location).
- `..` in a hex dump is a byte the model leaves unconstrained.
  `(default)` marks a value read from such bytes.
- A print that names a value by a string (`"end"`) is *weak*: it keeps
  nothing alive. Here the lowering removed `end`, which had no other use,
  and the print says `<dangling: end>` instead of guessing a value.
- A print with an SSA operand (`%s`) is *strong*: the value is kept alive
  through the lowering. `--print-refs=strong` turns every weak print into
  a strong one; with it, the same run prints `end = &heap.2[16] end`, a
  pointer one past the end of `heap.2`.

The full notation is on [Counterexample notation](../reference/print.md).
