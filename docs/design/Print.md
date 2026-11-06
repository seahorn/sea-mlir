# Counterexample printing: sea.print, sea.print.mem

Status: implemented.

Value-specific printing of counterexample values: ops name SSA values and
memory ranges; a run (sea-run, or sea-vc's replay of a sat model) renders
them: integers with their reading, memory as typed values or a hex dump,
structs as fields, pointers as object + offset with stable object names.
Prior art: Certora's cvlr-log (`CVT_calltrace_*` builtins, scopes) and its
consumer ctac (`AnnotationCmd` weak / strong refs), SeaHorn's `sea_printf`
and HexDump (`lib/seahorn/Smt/HexDump.cc`).

## 1. Ops

```mlir
sea.print "reached here"                        // a tag line
sea.print "amount" = "x" : i64                  // weak: the value named x
sea.print "delta" = "nd.3" : i64 {fmt = "s"}    // weak: nondet site 3
sea.print "amount" %x : i64                     // strong: an operand
sea.print "pt" %s : !sea.struct<i32, !sea.ptr<64>> {names = ["x", "next"]}
sea.print.scope "pre"
sea.print.mem "bal" = "nd.2"["p"] : i64 {fmt = "s", offset = 8}
sea.print.mem "buf" %m[%p], %n : !sea.mem<!sea.ptr<64>, i8>, i64 -> i8
    {fmt = "hexdump"}
sea.print.end "pre"
```

- `sea.print`: a tag line, or one value. `type` is the value's type; a
  struct prints one line per leaf, tagged by `names` (else 0, 1, ...).
  Formats (`fmt`): `auto` (default; cvlr's: unsigned decimal below 2^32,
  else hex, plus the signed value when it is negative and fits 32 bits;
  i1 as a bool), `u`, `s`, `x`, `b`, `c`, `bits`. Pointers and addresses
  always print as pointers (section 3).
- `sea.print.mem`: `count` elements (default 1; or the value of a count
  name / operand, capped at 65536) of `type` (whole bytes,
  little-endian, or a pointer / address), from `offset` bytes past the
  address, in the memory. `hexdump` (type i8) prints a dump. A struct in
  memory: a print scope plus one print.mem per field (no layout language).
- `sea.print.scope "t"` / `sea.print.end "t"`: tags nest as a dotted path
  (`pre.bal`); matched by tag at run time; an unmatched end is a
  `print.note:`, never a failure.
- `label` (optional) on `sea.alloca`, `sea.malloc`, `sea.global`: the
  producer's name for the object; `sea-place-static` copies it to the
  `sea.segment`. Shown in the object table.

Effects: a write to `sea::PrintResource` (not dead, keep their order); no
program state. `sea::isPrintOp`.

## 2. Weak and strong references (ctac's weak_refs / strong_refs)

Prints are weak or strong: use weak references when performance matters and
strong references when debugging requires the values to remain live.

- **Weak** (the names are string attributes): not uses. Every pass is free
  to fold, rewrite or delete the named value; no pass knows weak prints
  (they have no operands). Names (`sea::printNamesOf`):
  - `nd.<site>`: the result of the nondet op at that site (`sea.nondet`,
    `sea.struct.nondet`, `sea.ptr.nondet`), or the memory region of the
    `sea.mem.nondet` / `sea.memhavoc` at that site. Sites are stable
    through every pass.
  - the name of a NameLoc: `loc("x")` or `loc("x"(<file loc>))` on a block
    argument or the result of a one-result op (also a NameLoc directly in
    a fused location), as `sea-opt --mlir-use-nameloc-as-prefix` prints
    `%x`. Passes that rebuild an op at its location keep the name
    (placement's segments and addr.add, sea-split-wide-access's
    reassembled loads); a folded value (sccp's constants have no
    location) loses it.
  The interpreter keeps name -> most recent definition on the executed
  path and peeks it at the print; no definition, or one of another type
  (a pointer and an address of one width agree), prints
  `<dangling: x>`, never a guess. A memory name denotes the region (the
  Mem-SSA chain) of the named version; the print shows the region's
  contents at its point. Names live in locations: keep them between tools
  (`sea-opt --mlir-print-debuginfo`).
- **Strong**: the values (memory, address, count) are SSA operands, kept
  alive through every pass; passes treat the print as a user. A strong
  print.mem of a memory version that was consumed prints
  `<stale: an earlier memory version>` (the in-place interpreter has no
  old contents).
- `sea-print-strengthen`: each name resolves to the closest dominating
  value with that name (a memory name: the region's version current at
  the print: the last version before it in its block, else a block
  argument of the region, else the same in the immediate dominator). An
  unresolved name stays weak: warning `print.strengthen-unresolved`.
  `sea-print-weaken`: operands become names: `nd.<site>` first (a memory
  region starting at a sea.mem.nondet), else the NameLoc name if no other
  value has it, else a fresh NameLoc `print.<n>` on the value (renames its
  VC symbol; the query's meaning is the same). A strong print of a
  struct's leaves becomes a scope of leaf prints. `sea-vc` / `sea-run
  --print-refs=asis|weak|strong` runs one of them before the lowering.
- The ctac-like alias propagation (carry a weak name to the value that
  replaces it, ctac `rewrite/propagate_aliases.py`) is not implemented.

### Pass audit (strong form)

Every pass of `lib/Transforms` was run on programs with prints after
every value (`sea-test-insert-prints{mode=weak|strong|strong-all}`, a test
pass) over the VC tests. Needed changes:
- `sea-scalarize-structs`: a strong print of a struct shows its leaves
  (operands = leaves, `type` = the struct type); before, any other struct
  user was the error scalarize.struct-use.
- `sea-widen-memory`: a strong print.mem is not a reason to keep a memory
  as bytes (it refused any unknown memory user); the interpreter reads
  bytes out of k-byte cells.
- `sea-merge-asserts` and the BMC-form check (`verifyBmcForm`): "the
  assert right before the halt" skips prints (`sea::nextNonPrintOp`).
- OpSem: print ops encode nothing.
No change: placement (retypes operands in place; struct values are gone
by then), fat pointers and tracking (a print is a plain use of a pointer
or a memory), split-wide-access, expand-memset, drop-fresh-memhavoc,
expand-intrinsics, wrap-flags, check-align, check-ptr-index,
lower-ptr-index, split-critical-edges, apply-tracking, sccp (RAUW updates
strong operands; a folded weak name dangles), cse and canonicalize (the
lowering's clean-up, `sea-vc --lower-opt`: prints have a write effect, so
they are never erased or merged and strong operands are plain uses; a
weak name of a merged, folded or dead value dangles; folded constants get
an unknown location).

## 3. Pointers

An agent never does address arithmetic to learn a pointer's provenance.
Pointer notation:

| case | token |
|---|---|
| object + byte offset | `&heap.3[8]` (offset 0: `&heap.3[0]`) |
| one past the end | `&stack.5[16] end` |
| outside | `&stack.5[20] oob`, `&stack.5[-4] oob` |
| a freed object (abstract level) | `&heap.3[0] freed` |
| null, null + 8 | `null`, `&null[8]` |
| function | `&func.foo` |
| address without provenance | `0xbffffff8`, inside an object `0xbffffff8 ~stack.5[8]` (a hint) |
| byte range | `heap.3[0..20]` (half-open) |

Object names are the interpreter's and the segment map's (`stack.<site>`,
`heap.<site>`, `global.<name>`, `func.<name>`, `nondet.<site>`; `#n` for
the n-th execution of an allocation at the abstract level). Each object a
print refers to gets one line, before its first use:
`print.object: heap.3 kind=heap size=24 label=node at=foo.c:12:9 base=0x8048010`
(`at`: the allocation's file location, then ` inlined-at=<call
sites>` when it was inlined (docs/Release.md, "Where a counterexample
fails"), then `at_name=<name>` when the location has a name; a name-only location `loc("node")` gives its
position in the input .mlir file; `base`: placed objects).

Provenance after placement: the interpreter keeps a shadow object for
address bits: set by `sea.segment`, carried by `addr.add`, `ptr.index`
over an address, `select`, block arguments, `addr.to_bits` /
`addr.from_bits`, `extract` / `trunc` / `concat` of the bits (as bit
ranges of one address), and memory cells (stored bits keep their object;
a load of consecutive bits of one address gets it back). `addr.from_int`
has none (a pointer made from an integer has no provenance, docs
PtrSort.md). So the abstract run and the placed run print the same tokens
(test/Interp/print/agree.mlir); with objects back to back (SeaHorn's
layout places heap objects adjacently), one past the end of A is
`&A[16] end`, not `&B[0]`.

The same rendering: sea-run `--trace` (`value: ... = <bits> ptr=<token>`,
abstract pointers `= <token>`), `memread:` and replay `mismatch:` lines
(`obj=&heap.3[8]` for a concrete address inside a placed object: a
location, by range).

## 4. Running prints

- Interpreter (`RunOptions::prints`, `printLimit`): executes print ops on
  the executed path; nothing changes the run. A memory read is a peek
  (`NondetSource::peekCell`): never fails, never draws a random input,
  never recorded as a `memread`; `InputSource` gives an input or, with
  `--missing=zero`, 0, else nothing (`??`). `ModelSource` evaluates the
  model (`..` when the model leaves it unconstrained, origin default).
  A cell is unconstrained when no constraint of the query reads it
  (`solver::Model::defaultCell`): every use of its array in the query is
  a select (through stores and ites) and no select index has its address
  in the model. A solver's model gives such a cell a value anyway (an
  array's `else`, the 0 of an Ackermann / read-table function), so the
  evaluation alone cannot tell: `expr::ReadCellsModel` (the solver's
  arrays), `AckermannModel`, `xrt::TableModel` (their functions) and
  `WordsModel` (a byte is a default iff its word is) answer it, the same
  for every solver. Conservative: an array used other than by reads (an
  equality, a lambda), or a read index with no value, has no default
  cell; under `--seam-words` a word read covers all its bytes. The value
  is the model's (validation compares the same value). Values of default
  bytes get `(default)`. Register values derived from defaults are not
  marked (needs taint; later).
- sea-vc: the validation replay skips prints (its model evaluations and
  the `vc.ackermann-model` / `words.model` counters do not depend on
  them); a second run on the same model (`sea::replayPrints`) prints them,
  after the `replay:` lines and before `validate:`. The first `replay:`
  line stays exactly `replay: <status>` (harnesses parse it).
- The VC: OpSem skips print ops: no term. The query of a program is
  byte-identical with no prints, weak prints, and strong prints of used
  values (test/VC/print/vc-identical.test: --lower, --seam,
  --widen-memory=8). Strong prints of unused values keep them alive, but
  the VC encodes every op anyway: array_list_swap with a strong print of
  every value has the same query (38212 bytes).

## 5. Output

```
print: pre.amount = 4294967296 : ui64
print: delta = -2 : si64
print.object: heap.3 kind=heap size=24 label=node base=0x8048010
print: owner = &heap.3[8]
print: buf = heap.3[0..20]
print: buf +00: 00 00 00 00 01 00 00 00 ff ff ff ff ff ff ff ff |................|
print: buf +10: .. .. .. ..                                     |....|
print: gone = <dangling: x>
print.note: unmatched sea.print.end "t"
print.truncated: 1234
```

`--print=lines` (default: dotted scope paths, each line stands alone for
grep), `tree` (indented by scope, a line per scope), `off`;
`--print-limit=N` (default 1000 records; 0: none); `--print-locs` appends
`at=<file:line:col>`, then ` inlined-at=<file:line:col>[,...]` when the
print op was inlined (its call sites, outermost last), and
`at_name=<name>` when the print op's location has a name. Hex dump rows follow SeaHorn's HexDump / LLVM
`format_bytes_with_ascii`: 16 bytes, an ascii column, `*` for rows equal
to the one before (the last row is always shown); `..` a byte the model
leaves unconstrained, `??` a byte no source gives (or a byte of an
abstract pointer, which has no bits).

## 6. Later

- alias propagation for weak names (ctac);
- taint: mark register values derived from default inputs;
- float formats (f32 / bf16) and shaped arrays; strings; fixed point;
- a layout table for structs in memory, if producers emit many;
- front ends (SeaHorn exporter, sea_printf, cvlr `CVT_calltrace_*`).
