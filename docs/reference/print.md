# Counterexample notation

A program states what a counterexample should show with print operations.
They are evaluated whenever the program runs: in `sea-run`, and in the
replay of a `sat` by `sea-vc`. The query does not encode them; it is byte
for byte the same with and without them.

## Print operations

| operation | shows |
|---|---|
| `sea.print "tag"` | a line with the tag |
| `sea.print "tag" = "name" : <type>` | the value named `name` (weak) |
| `sea.print "tag" %v : <type>` | the operand `%v` (strong) |
| `sea.print.mem "tag" = "mem"["ptr"] : <type>` | memory at a pointer, by name (weak) |
| `sea.print.mem "tag" %m[%p], %n : <memory type>, <type> -> <type>` | memory, by operands (strong) |
| `sea.print.scope "t"` ... `sea.print.end "t"` | a group: tags inside print as `t.<tag>` |

Attributes:

- `fmt`: `auto` (default), `u`, `s`, `x`, `b`, `c`, `bits`, and `hexdump`
  for bytes.
- `count`: the number of elements of a memory print (default 1).
- `offset`: bytes past the pointer.
- `names`: the field names of a struct value.

`label` on `sea.malloc`, `sea.alloca` and globals gives an object a name
that is shown with it.

## Weak and strong

A weak print names its value by a string: a value whose location carries
that name (`loc("x")`), or `nd.<site>` for an input. It keeps nothing
alive. If a pass removed the value, the print shows
`<dangling: name>` instead of a guess.

A strong print has SSA operands, which the passes keep alive.
`--print-refs=strong` converts weak prints to strong ones before the
lowering, and `--print-refs=weak` does the reverse. Names live in
locations: keep them between tools with `sea-opt --mlir-print-debuginfo`.

## An example

```mlir
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %c16 = sea.const 16 : i64
  %c4 = sea.const 4 : i64
  %m0 = sea.mem.nondet "heap" [1] : !m loc("mem")
  %p = sea.malloc %c16 align 8 [2] label "buf" : i64 -> !sea.ptr<64> loc("p")
  %x = sea.nondet "nondet_i64" [3] : i64 loc("x")
  %q = sea.ptr.add %p, %c4 : !sea.ptr<64>, i64 loc("q")
  sea.print "reached here"
  sea.print "amount" = "x" : i64
  sea.print "delta" = "nd.3" : i64 {fmt = "s"}
  sea.print "amount.hex" %x : i64 {fmt = "x"}
  sea.print "q" = "q" : !sea.ptr<64>
  sea.print.scope "pre"
  sea.print.mem "words" = "mem"["p"] : i32 {count = 4}
  sea.print.end "pre"
  sea.print.mem "buf" %m0[%p], %c16 : !m, i64 -> i8 {fmt = "hexdump"}
  %c0 = sea.const 0 : i64
  %ok = sea.icmp eq, %x, %c0 : i64
  sea.assert %ok
  sea.halt
}
```

In the interpreter, with the input $-2$ and zero for unset memory:

```console
$ sea-run prints.mlir --input=3=-2 --missing=zero --print=tree
program: @main
nondet: site=3 callee=nondet_i64 width=64 value=18446744073709551614 origin=input
print: reached here
print: amount = 0xfffffffffffffffe : i64 (-2)
print: delta = -2 : si64
print: amount.hex = 0xfffffffffffffffe : i64
print.object: heap.2 kind=heap size=16 label=buf at=prints.mlir:6:8 at_name=p
print: q = &heap.2[4]
print: pre:
print:   words[0] = 0 : ui32
print:   words[1] = 0 : ui32
print:   words[2] = 0 : ui32
print:   words[3] = 0 : ui32
print: buf = heap.2[0..16]
print: buf +00: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 |................|
status: assert-failed
...
```

In the replay of `sea-vc`, after the lowering:

```console
$ sea-vc prints.mlir --lower
...
print: reached here
print: amount = 0xffffffffffffffff : i64 (-1)
print: delta = -1 : si64
print: amount.hex = 0xffffffffffffffff : i64
print: q = <dangling: q>
print.object: heap.2 kind=heap size=16 label=buf at=prints.mlir:6:8 at_name=p base=0x8048000
print: pre.words[0] = 0 : ui32 (default)
...
print: buf = heap.2[0..16]
print: buf +00: .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. |................|
validate: checked=5 mismatches=0 dontcare=0
```

The lowering removed `%q`, which had no other use, so the weak print of
`q` dangles. The memory was not constrained by the model: its bytes show
as `..`, and values read from them are marked `(default)`. After
placement, each object also shows its address (`base=`).

## Values

| form | meaning |
|---|---|
| `5 : ui32` | unsigned decimal, with the type's width |
| `-1 : si64` | signed (`fmt = "s"`) |
| `0xff : i8` | hexadecimal |
| `0xffffffffffffffff : i64 (-1)` | `auto` on a large value: hexadecimal, with the signed value when it is small and negative |
| `true` | an `i1` (a JSON boolean in `--format=jsonl`) |
| `(default)` after a value | read from bytes the model leaves unconstrained |

## Pointers

Pointers print as an object and a byte offset, never as a bare address
when the object is known.

| token | meaning |
|---|---|
| `&heap.3[8]` | object `heap.3`, byte offset 8 |
| `&stack.5[16] end` | one past the end of the object |
| `&stack.5[20] oob`, `&stack.5[-4] oob` | outside the object |
| `&heap.3[0] freed` | into a freed object |
| `null`, `&null[8]` | null, and null plus 8 |
| `&func.foo` | a function |
| `0xbffffff8` | an address without provenance |
| `0xbffffff8 ~stack.5[8]` | an address without provenance that lies inside an object (a hint) |
| `heap.3[0..20]` | a byte range, half-open |

Objects are named `stack.<site>`, `heap.<site>`, `global.<name>`,
`func.<name>` and `nondet.<site>`. Each object a print refers to gets one
`print.object:` line before its first use, with its kind, size, label,
allocation site (`at=`, with `inlined-at=` and `at_name=` when they apply;
see [Locations](output-formats.md#locations)) and, once placed, its
address (`base=`).

## Memory

A hex dump prints a header line and rows of 16 bytes:

```text
print: dump = heap.2[0..32]
print: dump +00: 64 00 00 00 00 00 00 00 10 80 04 08 00 00 00 00 |d...............|
print: dump +10: .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. |................|
```

`..` is a byte the model leaves unconstrained, `??` a byte that no source
gives, and `*` stands for repeated rows. `--print-limit=<n>` caps the
number of print lines (default 1000), and a `print.truncated:` line
counts the dropped ones. `--print-locs` appends the location of each print
operation, as `at=<pos> [inlined-at=...] [at_name=<name>]`.
