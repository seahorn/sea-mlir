# Constant data in memory: sea.mem.fill

`sea.mem.fill` (ODS: `include/sea/Dialect/Sea/SeaOps.td`; verifier:
`lib/Dialect/Sea/SeaOps.cpp`, `sea::verifyMemFillData`), its interpreter
execution (`lib/Interp/Interpreter.cpp`, `execMem`), its VC lowering
`sea-expand-mem-fill` (`lib/Transforms/ExpandMemFill.cpp`) and its
interaction with `sea-widen-memory` (`lib/Transforms/WidenMemory.cpp`).
The seaimp region form is `seaimp.mem.fill` (docs/design/SeaImp.md).
Under `--seam` a fill of a byte memory is one term, `mfill`
(`include/sea/Expr/MemOps.hh`, `SeaMRules.hh` `rowFill`; section "SeaM"
below).
Tests: `test/Dialect/Sea/mem-fill.mlir`,
`test/Dialect/Sea/mem-fill-invalid.mlir`, `test/Interp/mem/fill.mlir`,
`test/Transforms/expand-mem-fill.mlir`, `test/VC/mem-fill.mlir`,
`test/VC/mem-fill-4k.test`, `test/VC/seam/mfill-*.mlir`,
`test/VC/seam/mfill-4k.test`, `test/SeamExport/mfill.mlir`, the `mfill`
cases of `unittests/MemOps` and `unittests/SeaMRules`.

Status: implemented. Without `--seam` the VC sees stores; with `--seam`
a fill of a byte memory is a term, the others are stores.

## The op

```mlir
%m1 = sea.mem.fill %m0[%p], dense<[1, 2, 3]> : tensor<3xi8> inplace : !sea.mem<!sea.ptr<64>, i8>
```

Cell `k` of the data is written at `%p + k * c`, where `c` is the number
of index units per cell (1 for the exact and keyed layouts, `k` for
`k`-byte words). It is a memcpy from constant data: it models memory
preloaded with values, such as an LLVM global initializer. It is not a
memset, which repeats one value.

- The data is a builtin dense integer attribute (`DenseIntElementsAttr`):
  any cell width, a splat form (`dense<0> : tensor<4096xi8>`), compact
  storage and a hex form for large initializers.
- The verifier requires 1-D data of the memory's cell type, at least one
  cell, cells that fit the index space (`N * c < 2^w`, so no fill wraps
  onto its own cells), and an index of the memory's index type.
- It is an update like the others: it consumes a memory and makes the
  next version, with `inplace` and `tracked`. A tracked fill marks the
  object of `%p` as modified (it always writes at least one cell).

The interpreter writes the cells in place, after the range check of the
whole fill (an out-of-bounds fill is a memory-safety error); the A-INB
job (`sea-check-inb`) checks its range once, and `sea-check-ptr-index`
treats it as an access of its address.
Under deferred memory errors an out-of-object fill writes nothing, while
its store expansion writes the in-object cells. Memset and its expansion
differ the same way; this only matters for runs that are already
inconclusive.

## The VC: stores of the memory's cells

`sea-vc --lower` runs `sea-expand-mem-fill` right after
`sea-widen-memory`: each fill becomes one store per cell, in order, of
the memory's cell type, each the constant cell of the data. The first
store consumes the fill's memory with the fill's `inplace`; the others
consume a version only the chain uses. The arrays, `--mem=lambda` and
word layouts see stores; `--seam` sees the stores of word and keyed
memories and an `mfill` term for each fill of a byte memory (section
"SeaM"). Without `--lower` the VC refuses a fill (`vc.mem-fill`), except
a byte-memory fill under `--seam`.

With `keep-bytes` (sea-vc `--seam`) the pass keeps the fills of byte
memories (`MemType::isByteCells`: i8 cells of one index unit, not keyed)
and expands the others. Its counters, printed by sea-vc for a program
with a fill: `vc.expand-mem-fill: expanded=<fills> cells=<stores>
kept=<fills> expanded-word-layout= expanded-keyed= expanded-other=` (the
last three: fills expanded under `keep-bytes`, by why they are not
terms).

The expansion is exact: the stores write the cells the fill writes, with
its values, and nothing else. It does not rest on any assumption.

### Widening

The stores have the memory's final cell type: on a byte memory, one i8
store per byte; on a memory of 8-byte cells, one i64 store per word.
`sea-widen-memory` decides the cells with the fill still whole:

- a fill qualifies for `k`-byte words if it starts at a provably
  `k`-aligned address (an allocation or global aligned to `k` plus
  constant offsets that are multiples of `k`, a placed segment, ...) and
  has a multiple of `k` bytes;
- a fill that starts or ends inside a word disqualifies its memory for
  that word size, as a store narrower than a word does: the memory is
  kept at a smaller word size it qualifies for, or as bytes (the widening
  report says `a fill not provably word-aligned` or `a fill length not a
  multiple of a word`). There is no partial-word write, so no
  read-modify-write is needed;
- on a widened memory, the fill's bytes are packed into words,
  little-endian (byte `i` of word `j` is byte `j * k + i`), as a `k`-byte
  store of those bytes would write them.

`sea-widen-memory` runs after placement in the lowering, so
`sea-drop-fresh-memhavoc`, `sea-lower-tracking`, `sea-lower-fat-pointers`
and `sea-place-static` see the fill itself. They handle it: a write of
its object (drop-fresh), a mark of its object when tracked
(lower-tracking), integer data that leaves the pointer companions
unchanged, as an integer store does (lower-fat-pointers), a retyped
operand (placement).

### Size

A 4 KiB initializer (4096 bytes on an 8-aligned global, one 8-byte load
at a nondet aligned offset; `test/VC/mem-fill-4k.test` builds the same
shape), measured with the expansion (sea-vc `--lower --vcgen-only
--dag-stats`, z3 with a 120 s limit):

| mode               | DAG nodes | SMT-LIB bytes | VC build | solve (z3)       |
|--------------------|----------:|--------------:|---------:|------------------|
| arrays (default)   |    16 697 |       497 034 |   0.47 s | > 120 s (timeout) |
| `--mem=lambda`     |     4 246 |       169 968 |   22.7 s | 0.02 s           |
| `--widen-memory=8` |     3 101 |        68 318 |   0.09 s | > 120 s (timeout) |
| `--seam`           |     6 158 |       494 168 |   1.62 s | 0.05 s           |

The query grows linearly with the data (one store per cell). The lambda
build spends its time in `lambda.simplify-time` (22.8 s): its
simplification of a chain of stores is quadratic in the chain's length,
a property of the lambda encoding of long store chains, not of the fill.
z3 does not answer the array query (4096 or 512 nested stores read at a
symbolic index) within 120 s, while SeaM's rewriting of the read and the
lambda form answer at once.

## SeaM: the `mfill` term

With `--seam`, `sea.mem.fill %m[%p], <data>` on a byte memory is

    mfill(M, d, n, data)    cell j:  (j - d) <u n ? data[j - d] : M[j]

(`MEM_FILL`, `include/sea/Expr/ExprOpArray.hh`): M the incoming memory,
d the destination, n the number of bytes as a numeral at the index width
(the verifier gives 0 < n < 2^w), data the bytes (a `FillData` terminal,
compared and hashed by content). It shares the range machinery of `mset`
/ `mcopy` / `mhavoc` (SeaM-memops.md): `memOpDst` / `memOpLen` are d and
n, the destination range is an access (`accessRange`: counted, its
A-INB condition stated, a static violation reported), cross-object
decisions are `decideIn`'s with the premises it names (A-SEP), the
firing premise states the range (`accessRanges`), and a shadow memory
gets no premise. The model is seam-lean's `rangeFill`
(`SeaM/MemOps/Basic.lean`; its `docs/MemFill.md`): the cell at circular
distance k from d holds byte k when k < data.length, and keeps its value
otherwise. Scope, as there: byte memories, full bytes (no holes).

The rules (`SeaMRules.hh`; the counter is the rule name in the trail and
in `--seam-check`), each with the theorem it corresponds to:

| rule | when | result | seam-lean theorem |
|---|---|---|---|
| `row.mfill-hit` | k = j - d a numeral, k < n | the numeral data[k] | `RangeFill.hit_sound` (r = 1), `rangeFill_hit` |
| `row.mfill-skip` | k a numeral, k >= n | select(M, j) | `RangeFill.skip_sound` (r = 1), `rangeFill_skip` |
| `row.mfill-skip-disjoint` | j decided outside [d, d + n) by other means: another object (`cmp.diff-base`, `arm.skip`: A-SEP), an ARM interval of j - d outside the range | select(M, j) | `RangeFill.skip_disjoint_sound`, `rangeFill_skip_inRange` |
| `row.mfill-split` | j or d an ite whose every leaf pair is decided by the three rules above | the ite of the leaf results (each leaf counted under its rule) | per leaf as above; the split is the ite of reads (exact) |
| `row.mfill-expand` | anything else: an open in-test, or in the range at an offset that is not one numeral | select(fillStores(mfill), j): the stores the fill expands into, decided by the store rules per byte | `RangeExpr.eval_mfill_eq_fillStores`, `rangeFill_eq_fillStores` |
| `wide.mfill-hit` | a wide read of r bytes (`--seam-wide`) at a numeral k, k + r <= n | the numeral of bytes k .. k + r - 1, little endian | `wide_fill_hit`, `RangeFill.hit_sound` |
| `wide.mfill-skip` / `-skip-disjoint` | a wide read decided outside the range (numeral k / otherwise) | wselect(M, j) | `wide_fill_skip` / `wide_fill_skip_of_disjoint` |
| (byte fallback) | a wide read across the range's end or start, or in it at a symbolic offset | its byte reads (`wide.split-read`), each by `row.mfill-*` | `RangeFill.mixed_sound` at a numeral k (each byte decided); else the expansion |
| `mem.fill-replicate` | every byte of the data is one byte b | mset(M, d, n, b) | `rangeFill_replicate` |
| boundary | a read over the term left (rules off, interrupted) | ite((j - d) <u n, lookup(j - d), select(M, j)) | the definition `rangeFill` (`rangeFill_hit`, `rangeFill_skip`) |

`rangeFill_take` (only the first 2^w bytes matter) and the bound of
`rangeFill_eq_fillStores` (n <= 2^w) need nothing here: the op verifier
gives n < 2^w (`N * c < 2^w`), so the term is the whole data.
`rangeFill_append` (a fill split in two) is not used by a rule.

### The fallback: the store expansion

A read whose offset from d is not decided falls back to the store
expansion of that read, not to the range test of the other terms. The
range test `ite((j - d) <u n, <hit>, M[j])` would need a hit form for a
symbolic offset: a lookup of `j - d` in the data, a term the Lean model
does not have (its scope covers decided offsets; every other read keeps
the expansion, justified by `rangeFill_eq_fillStores`). The expansion is
the same stores `sea-expand-mem-fill` makes, as a memory term
(`fillStores`, MemOps.hh: byte i at d + i, in order), one node per fill
(hash-consed: every undecided read of the fill reads the same chain, and
the rewriter normalizes it once). So an undecided read costs what every
read costs without the term, and a decided read costs one rule. A fill of
one repeated byte (a zero initializer) is an `mset` instead
(`mem.fill-replicate`), whose undecided read is one range test.

### The boundary

`solverTerm` (`MemOpTranslator`, MemOps.hh) removes a read over an
`mfill` left in the query by the read equation of the term,

    select(mfill(M, d, n, data), j) = ite((j - d) <u n, lookup(j - d), select(M, j))
    lookup(k) = ite(k = 0, data[0], ite(k = 1, data[1], ... data[n-1]))

which is seam-lean's definition `rangeFill` (distance below the length:
the data byte; else the old cell). Semantics only: no premise, no address
decision; counted in `memop.translated`. Only a run with the rules off or
stopped leaves such a read.

Not the store expansion, which seam-lean's `RangeExpr.lower` uses
(`RangeLowered.fillChain`, `RangeExpr.lower_mfill`). The two are equal
(`rangeFill_eq_fillStores`); the read equation was chosen for
`--seam-check`, which sends both sides of every firing through this
boundary. Measured on the obligations a firing over a fill makes (z3,
64-bit addresses, the destination a placed constant):

| obligation | n = 16 | n = 256 | n = 4096 |
|---|---:|---:|---:|
| skip of a read past the data, the boundary the store chain | 0.01 s | 0.04 s | 21 s |
| the same, the boundary the read equation | 0.02 s | 0.04 s | 1.1 s |
| the store chain = the read equation (`row.mfill-expand`) | 0.02 s | 2.0 s | > 30 s |

and `mem.fill-replicate` of 64 bytes, a store chain against a range
test: z3 > 60 s, Yices 22 s, Bitwuzla 19 s (the read equation against
the range test: 0.01 s). With the store chain as the boundary, the
expansion rule would be valid by construction and every decided rule,
the ones that use premises, expensive to check; with the read equation
the decided rules and the replicate rule check at once, and the check of
a `row.mfill-expand` firing is a real instance of
`rangeFill_eq_fillStores`, which z3 does not settle within the default 10
s beyond a few hundred bytes (`seam-check-unknown`, never `invalid`; the
4 KiB read of `test/VC/mem-fill-4k.test`: 98,336 firings, 0 invalid, 9
unknown, 8 of them `row.mfill-expand` and one `row.smap-abort` on the
expansion's stores). The unit test checks the read equation and
the expansion against an independent guarded-store reference for every
length at 4-bit addresses (`unittests/MemOps`, `fill`).

### Export

`sea-seam-export` writes the term as `{op: mfill, sort: memory, args:
[memory, dst], data: [b0, b1, ...]}`, bytes as integers 0..255 (seam-lean
`RangeExpr.mfill m dst data` / `MapExpr.mfill`: no length argument, the
length is the data's; the exporter checks that Expr's numeral n equals
it). Schema 5, an added op.

### Size

`test/VC/seam/mfill-4k.test`: a 4 KiB initializer read at decided
offsets (an 8-byte load and a byte) and a 4 KiB zero initializer read at
a symbolic offset: `vc.dag-size` 19 under `--seam` (no store of either
fill: nine `row.mfill-hit`, one `mem.fill-replicate` and its one
`row.mset` range test), 24,889 for the expansion (`--lower` without
`--seam`). A read at a symbolic offset of varied data costs the
expansion, as before: the 4 KiB program of `test/VC/mem-fill-4k.test`
(an 8-byte load at a symbolic aligned offset) is 6,146 nodes under
`--seam` (6,158 before; RelWithDebInfo, `--lower --seam --dag-stats`).

## Later

- A read in the range at a symbolic offset (a table lookup `tab[i]` with
  i bounded) falls back to the expansion. A lookup term (an ite over the
  bytes the offset's interval admits, or a constant array) would make it
  one range test; it needs a Lean counterpart first.
- `rangeFill_append`: split a fill at long runs of one byte (mset) and
  the rest (mfill); and fills of word memories (SeaM-memops.md step 3).
- The SeaHorn export switches global initializers from byte stores to
  `sea.mem.fill`.
