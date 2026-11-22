# Constant data in memory: sea.mem.fill

`sea.mem.fill` (ODS: `include/sea/Dialect/Sea/SeaOps.td`; verifier:
`lib/Dialect/Sea/SeaOps.cpp`, `sea::verifyMemFillData`), its interpreter
execution (`lib/Interp/Interpreter.cpp`, `execMem`), its VC lowering
`sea-expand-mem-fill` (`lib/Transforms/ExpandMemFill.cpp`) and its
interaction with `sea-widen-memory` (`lib/Transforms/WidenMemory.cpp`).
The seaimp region form is `seaimp.mem.fill` (docs/design/SeaImp.md).
Tests: `test/Dialect/Sea/mem-fill.mlir`,
`test/Dialect/Sea/mem-fill-invalid.mlir`, `test/Interp/mem/fill.mlir`,
`test/Transforms/expand-mem-fill.mlir`, `test/VC/mem-fill.mlir`,
`test/VC/mem-fill-4k.test`.

Status: implemented. The VC sees stores; no memory encoding handles a
fill natively yet.

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

## The VC: stores of the memory's cells

`sea-vc --lower` runs `sea-expand-mem-fill` right after
`sea-widen-memory`: each fill becomes one store per cell, in order, of
the memory's cell type, each the constant cell of the data. The first
store consumes the fill's memory with the fill's `inplace`; the others
consume a version only the chain uses. Every memory mode (arrays,
`--mem=lambda`, word layouts, `--seam`) then sees stores. Without
`--lower` the VC refuses a fill (`vc.mem-fill`).

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

## Later

- A native fill in the memory encodings: a constant-array term (or
  SeaM's store-map) read through one range test, as `mset` / `mcopy` do
  for memset and memmove (docs/design/SeaM-memops.md). It needs a
  coordinated SeaM change; until then every mode sees stores.
- The SeaHorn export switches global initializers from byte stores to
  `sea.mem.fill`.
