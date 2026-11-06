# Memory, step 1: concrete memory

Status: types, ops, in-place interpreter and the array encoding
(including constant-length memset/memmove/memhavoc) are implemented; the lambda encoding is deferred (naive lambdas do not work
well; it needs SeaHorn's beta-reduction and simplification techniques). The
static `inplace` checker pass is not written: the interpreter checks the
promise exactly on the path it executes.

## Scope

Step 1 is the *concrete* level: what the VC generator consumes. Addresses
are values of an address type; memories map addresses to cells; there are
no abstract pointers, no allocation, no alignment rules. Later steps add
the abstract level (`!sea.ptr`, allocation) and lowering passes (raw
pointers, memcpy -> memmove, unaligned split, copy expansion, address-space
retargeting), all MLIR -> MLIR.

## Principles

1. **Explicit def-use chains (Mem-SSA, one chain per region).** A memory is
   an SSA value. An op that changes memory takes a version and returns a new
   one. Merges are block arguments. No MLIR side effects on memory.
2. **One semantics, two executions.** Semantically every version is a new
   map. The interpreter executes chains in place, one buffer per chain, and
   accepts only programs whose memory-consuming ops are all marked
   `inplace` (below).
3. **The VC encodes exactly or refuses.** No unrolling, no approximation in
   vcgen. A representation that cannot encode an op exactly gets a lowering
   pass first.
4. **No alignment in the VC.** An access at address `a` reads or writes the
   cell at `a`. Whether cells overlap is the business of the unaligned-split
   pass and the representation, not of the semantics.

## Types

### `!sea.addr<W>` / `!sea.addr<W, unit = U>`

An address: a `W`-bit bit-vector (W in 1..64). `U` is the number of bits one
address unit covers (default 8: byte addressing); `+1` moves one unit.
Interpreter: an APInt of width W. VC: `(_ BitVec W)`. Bit-vector arithmetic
ops do not accept addresses.

### `!sea.mem<A, C>`

A memory: a map from addresses of type `A` (an `!sea.addr`) to cells of
type `C` (a signless integer `iN`). Interpreter: a buffer (sparse map
address -> cell) over a base (see `sea.mem.nondet`). VC:
`(Array (_ BitVec W) (_ BitVec N))`.

A cell is the unit of load and store. SeaHorn's representations map to it:
byte memory `!sea.mem<!sea.addr<64>, i8>`; 4- or 8-byte cells
`!sea.mem<!sea.addr<64>, i64>` (byte-addressed, cells at aligned addresses
by convention of the lowering); word-addressed memory
`!sea.mem<!sea.addr<61, unit = 64>, i64>`.

## Operations

Notation: `m` a memory version, `a` an address, `n` a length in units
(an `iW` with W the address width), `mod 2^W` arithmetic on addresses.

The range of an op `[a, a + n)` is the set of addresses `i` with
`(i - a) mod 2^W <u n`. It wraps consistently; the VC tests membership with
one `bvult(bvsub(i, a), n)`.

### Addresses

| op | semantics |
|---|---|
| `%q = sea.addr.add %p, %off : !sea.addr<W>, iW` | `p + off mod 2^W` (offset in units) |
| `%d = sea.addr.diff %p, %q : !sea.addr<W>` | `p - q mod 2^W`, an `iW` |
| `%c = sea.addr.cmp ult, %p, %q : !sea.addr<W>` | like `sea.icmp`, an `i1` |
| `%i = sea.addr.to_int %p : !sea.addr<W> to iW` | the bits of `p` (escape) |
| `%p = sea.addr.from_int %i : iW to !sea.addr<W>` | the address with bits `i` |

`sea.select` also accepts addresses. There is no address constant; use
`sea.addr.from_int` of a `sea.const` (the null address is 0).

### Memories

| op | semantics |
|---|---|
| `%m = sea.mem.nondet "<name>" [<site>] : !sea.mem<A, C>` | unconstrained initial contents of a region; `site` is unique per program (shared with `sea.nondet` sites) and is the replay key |
| `%v = sea.load %m[%a] : !sea.mem<A, C> -> T` | `m[a]`; `T` is `C` or an `!sea.addr` of C's width |
| `%m1 = sea.store %m[%a], %v : !sea.mem<A, C>, T` | `m[a := v]` |
| `%m1 = sea.memset %m[%a], %v, %n : !sea.mem<A, C>` | `λi. i in [a, a+n) ? v : m[i]`; `v : C` (a cell, not a byte) |
| `%m1 = sea.memmove %d[%da], %s[%sa], %n : !sea.mem<A, C>` | `λi. i in [da, da+n) ? s[sa + (i - da)] : d[i]`; `s` and `d` may be the same version or different regions of the same type |
| `%m1 = sea.memhavoc %m[%a], %n [<site>] : !sea.mem<A, C>` | `λi. i in [a, a+n) ? h[i] : m[i]` with `h` a fresh memory named by `site` |

Notes:
- `memset` takes a cell value. Replicating a byte into a wider cell
  (C's `memset`) is the lowering's job (SeaHorn does it inside MemSet).
- `memmove` reads the source version as it was before the op, so overlap
  is harmless (one semantics for memcpy and memmove; the memcpy
  precondition is a pass).
- `memhavoc` makes only the range nondeterministic, unlike SeaHorn, where
  the whole region gets a fresh version. The site is the replay key.
- Loads and stores of addresses store their bits (`T = !sea.addr<N>` with
  `C = iN`); loading back as `iN` is allowed (type punning).

## In place: the `inplace` attribute

A *consumer* of a version `m` is an op that produces a new version from it
(`store`, `memset`, `memmove` as destination, `memhavoc`) or a branch that
passes `m` to a block argument. `memmove`'s source and `load` are readers.

Memory ops are persistent (each returns a new version; the old one stays
valid). The unit attribute `inplace` on a consumer promises that its memory
operand is dead after it, so the result may reuse the operand's storage:

```mlir
%m1 = sea.store %m0[%a], %v inplace : !sea.mem<!sea.addr<64>, i8>, i8
```

- The VC ignores the attribute (same semantics).
- The interpreter accepts only programs in which every consumer is
  `inplace` (and every block argument merge of memory is a consumer of a
  dead version); anything else is refused with an error. No copies, no
  copy-on-write in the interpreter.
- A verifier pass checks the promise: on every path, a version consumed by
  an `inplace` op has no other consumer.
- The SeaHorn exporter sets `inplace` everywhere (Mem-SSA guarantees it).
  Converting persistent programs (choosing where `inplace` holds, and
  later inserting copies) is Joseph's liveness algorithm (LPAR'24), a pass.
- The conditional in-place write (Btor2MLIR `ite_write_in_place`) comes from
  `select` on memories, which SeaHorn does not produce; not in step 1.

Loads of a version after its consumer (optimized Mem-SSA: "the load's
address was not clobbered since `m`") are allowed. The in-place interpreter
then reads the current buffer, the VC reads `m`; they agree exactly when
the no-clobber claim holds, and replay validation reports a mismatch
otherwise: sea-vc names each such load whose value mismatches, or that a
mismatched value depends on, `replay: no-clobber-violated: <loc>`, and
vc.replay-mismatch (exit 2) says the cause is the claim, not the VC. A
strict mode of the verifier forbids such loads, for programs that do not
rely on the claim (not written, as the `inplace` checker pass itself; the
interpreter warns interp.load-earlier-version at each such load it runs).


## Interpreter

- Memory values are handles to buffers. An `inplace` consumer mutates the
  buffer and binds its result to the same handle.
- A buffer is a sparse map address -> cell over a *base*. `sea.mem.nondet`
  and `sea.memhavoc` create bases; a read that misses the written cells asks
  the base. Bases come from the `NondetSource`: inputs (`--input` gets a
  way to give cells), zero, random, or the solver model (lazy replay:
  evaluate `select(mem.<site>, a)` in the model).
- `memmove` copies through a temporary when the source handle is the
  destination handle (overlap).
- Replay validation compares loaded values with the model (as for scalar
  values); memory versions themselves are not compared.

## VC encoding

Two encodings, chosen per run, never mixed (as SeaHorn's array and lambda
memory representations, `--horn-bv2-lambdas`):

**Array encoding** (QF_ABV). A memory is `(Array (_ BitVec W) (_ BitVec
N))`.

| op | Expr |
|---|---|
| `mem.nondet`, `memhavoc`'s `h` | array constant `nd.<site>` |
| `load` | `select(m, a)` |
| `store` | `store(m, a, v)` |
| `memset`, `memmove`, `memhavoc`, constant length n | an exact chain of n `store`s (as SeaHorn's array representation does for concrete lengths); `memmove` reads `select(src, sa + k)` from the source version before the op; `memhavoc` reads from a fresh array `nd.<site>`. Warns above 1024 cells, refuses above 65536. |
| same, symbolic length | not expressible: refused with an error (`vc.mem-op-symbolic-length`); needs an expansion pass (bounded, with an assert) or the lambda encoding |

Memory block arguments use the equality encoding (`edge => m_join =
m_pred`), as for scalars.

**Lambda encoding.** A memory is a function from addresses to cells,
built as Expr lambdas and compiled away in Expr before the solver (beta
reduction, `bind::betaReduce`), so no lambda reaches any solver: z3 does
not compile lambdas efficiently, and lambda-free bit-vector queries allow
head-to-head comparison of bit-vector procedures with different array
support.

| op | Expr |
|---|---|
| `mem.nondet`, `memhavoc`'s `h` | base memory: array constant `nd.<site>`, read with `select` only |
| `store` | `lambda i. ite(i = a, v, m(i))` |
| `memset` | `lambda i. ite(bvult(i - a, n), v, m(i))` |
| `memmove` | `lambda i. ite(bvult(i - da, n), s(sa + (i - da)), d(i))` |
| `memhavoc` | `lambda i. ite(bvult(i - a, n), h(i), m(i))` |
| `load` | `m(a)`, beta-reduced |

Memory block arguments cannot use equalities (that would equate lambdas;
SeaHorn warns about it): they merge with `ite` over the incoming edge
conditions (SeaHorn's `defPHINodesIte`). This relies on the edges of a
block being mutually exclusive, which holds because `cond_br` targets are
distinct (BMC form).

## Verifier rules (step 1)

- `load` result / `store` value: `C`, or `!sea.addr` of C's width.
- `addr.add` offset, `addr.diff` result, lengths: `iW`, W = address width.
- `memmove`: source and destination memories have the same type.
- Sites unique per program across `sea.nondet`, `sea.mem.nondet`,
  `sea.memhavoc`.
- Linearity (verifier pass, above).

## Tests and oracles

- Roundtrip and verifier tests per op (disposable).
- `utils/check-semantics.py` gains the address ops (pure).
- `utils/fuzz-vc.py` gains memory: random chains with stores, memset,
  memmove (overlapping and cross-region), memhavoc, loads of current and
  earlier versions (no-clobber cases on purpose, and clobbered ones to see
  replay report the mismatch), merges at joins, constant and symbolic
  lengths. Oracles as for scalars: z3 and Yices agree, sat replays with no
  mismatch, unsat survives random runs.
- Planted bugs to confirm the oracle: off-by-one in the range test, memmove
  reading the updated source, memset ignoring `n`, linear interpreter
  without the overlap temporary.
