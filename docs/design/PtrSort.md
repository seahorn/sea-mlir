# A pointer sort in Expr

Status: implemented. Memories are indexed by `ptr(w)` (D1), pointer
equality is `PTR_EQ` (D6), and there is no reprovenance rule (D17).
A pointer can carry a base, offset, and metadata; its address alone need
not preserve that information.

## Why

Before the sort every address in the VC was a `bv(n)` term: pointer
arithmetic, integer arithmetic on addresses and integers were the same
terms, SeaM rediscovered pointer structure from `bvadd` / `bvmul`, and a
promise (inbounds, align) survived only as long as a term shape did. With
the sort the type checker enforces the separation: `BADD` on a pointer is a
type error, every way into and out of the sort is an explicit operator, and
the rules read structure instead of guessing it.

## Two pointer modes

Address mode admits integer-to-pointer conversion. Opaque mode rejects it
so that its pointer model can retain stronger provenance assumptions.

`sea-vc --ptr-mode=auto|opaque|address` (default auto), classified on the
input program before the lowering: address mode iff the program has an
int-to-ptr (`sea.ptr.from_int` / `sea.addr.from_int`). Printed in every run
(`vc.ptr-mode`, `vc.int-to-ptr` = program ops, `vc.ptr-from-int-opaque` =
encoded `PTR_FROM_INT`s; also BRUNCH stats).

| | opaque | address |
|---|---|---|
| who | regular C (vcc-pi2: 226 of 228) | low-level code (vcc-pi2: byte_cursor_advance_nospec{,2}, `from_int(to_int(p) & m)`) |
| int-to-ptr | `--ptr-mode=opaque` refuses the program (`vc.int-to-ptr`, at the op); reaching the encoder anyway is the same error (a bug) | `PTR_FROM_INT(v)`: a pointer with no provenance, no view, no A-INB leaf, counted |
| ptr-to-int | `PTR_TO_INT(p)`, the escape | same |
| what applies later | the fat model (S6: loaded-pointer reconstruction), A-PTRINT, `IS_OVERLAP` | the sort, the rules, the erasure, A-INB |

Everything but the `addr.from_int` encoding is mode-blind: one sort, one
rule set, one erasure (OpSem::setAddressMode, S2f).

## The sort and its operators (ExprOpBv.hh, ExprOpPtr.hh)

`ptr(w)` (`op::ptr::ptrsort`, a `Terminal<PtrSort>`, TerminalKind
PTRSORT): a pointer into a w-bit address space. `op::bv::width` is for
`bv(w)` only (it asserts); an address width is `ptr::width` /
`ptr::addrSortWidth` / `ptr::addrWidth(a, tc)`.

Constructors (every way a pointer comes to exist):
- `obj.k : ptr(w)` (`ptrConst`): a placed object (the segment), with SeaM.
  Its address is the layout side condition `ADDR(obj.k) = <numeral>`.
  Without SeaM a segment is `PTR_FROM_INT(<numeral>)`.
- ptr constants: registers of `!sea.addr` values (block arguments, nondet
  pointers after placement).
- `PTR_INDEX*(p, x_k, s_k, c, K)`: the only pointer arithmetic
  (PtrIndex.md): `p : ptr(w)`, `x_k : bv(w)`, result `ptr(w)` (strict).
  `addr.add` and `sea.ptr.index` both build one (`addressPlus`, which
  merges numerals into an unflagged node and writes K at a promised one).
- `ITE` at the pointer sort.
- `PTR_FROM_BITS(v : bv(w))`: a pointer loaded from memory (every load of
  an address value, wide or one-cell); `ptr.load-resolve` reads it back to
  the stored pointer when a ROW hit finds `PTR_TO_BITS(p)`.
- `PTR_FROM_INT(v : bv(w))`: int-to-ptr, provenance lost (address mode).

Observations (the only ways out):
- `ADDR(p) : bv(w)`: the address, at ordered compares, the layout facts
  and the integer positions the rules build (range conditions, `diff.erase`).
- `PTR_TO_INT(p)`: the escape (`sea.addr.to_int`).
- `PTR_TO_BITS(p)`: a stored pointer's bits (every store of an address).
- `PTR_DIFF(p, q) : bv(w)`: `sea.addr.diff`; rules diff.self /
  diff.same-base / diff.ite-same-base, else `diff.erase`.
- `OFFSET(x_k, s_k, c, K) : bv(w)`: a node's offset without its base, as
  a view returns it.
- `PTR_EQ(p, q) : Bool`: pointer equality = address equality (D6);
  `sea.addr.cmp eq / ne` and the eq join of pointer block arguments.
  Ordered compares are bit-vector compares of `ADDR`s. SeaM's `ptrEq`
  applies `eq()`'s pointer rules in the program context (no A-INB: a
  one-past-the-end pointer may equal the next object's base), cancels a
  common numeral (`ptr.eq.cancel-num`: `PTR_EQ(p + r1, q + r2)` with r1,
  r2 of one sign becomes the compare with the one nearer 0 subtracted
  from both; r is K of a flagged node, c + K of an unflagged one, the
  common r of an ite's leaves; exact, addition mod 2^w) and orders the
  sides (`ptr.eq.order`); the bit-vector compare rules do not apply.
- `IS_OVERLAP(p, n, q, m) : Bool` (D13): do the n units at p and the m
  units at q share an address (on the circle: `addr(q) - addr(p) <u n` or
  `addr(p) - addr(q) <u m`)? An alias question of two accesses: the
  compare of a ROW step that the rewriter left open (rowCompare, so also
  store-map R-ABORT entries; W-ROW-CONGRUENT / -ALIGNED) is
  `IS_OVERLAP(p, 1, q, 1)`. It stays an `IS_OVERLAP`; it is never reduced
  to a `PTR_EQ`. SeaM's `overlap` decides it in the access context, as ROW
  decides the compare it came from (`decideEq` with diffBase): different
  objects do not overlap, whatever the offsets (`cmp.diff-base`, ARM
  across bases, `cmp.size0-vacuous`; uses A-INB, counted, and stated by
  `--seam-check` when both pointers are bytes of recorded accesses). Then
  the ite push with the open leaves marked again, one object's offsets
  compared (`cmp.same-base`, recorded for the census), the numeral
  cancellation, and the order of the sides (`ptr.overlap.order`).
  `ptr.overlap.cancel-num`, only for n = m = 1:
  `IS_OVERLAP(p + r1, 1, q + r2, 1)` becomes
  `IS_OVERLAP(p + (r1 - t), 1, q + (r2 - t), 1)` (r and t as for
  `ptr.eq.cancel-num`; exact on addresses). Byte k of two accesses then
  asks as their starts: the eight unit overlaps
  `overlap(p + K=k, 1, q + K=k, 1)` of a split pointer are one term. A
  cancelled pointer need not be an access address (p + r1 is the access,
  p + r1 - t maybe not), so the access context may not read an in-object
  range of it as a fact. It does not: what it adds to the program context
  is object identity only (the bases of the two views, the provenance of
  each ARM leaf; ARM's intervals are exact claims about the term). The
  cancellation keeps the base of every leaf, so a decision of the
  cancelled question is a decision about the two accesses. Shadow
  memories (keyed by object identity, not accessed) keep their compares
  as `PTR_EQ`. What stays open is erased to the address `EQ` (unit
  widths), so the query is the one before. Keeping alias questions
  explicit allows unresolved ones to be counted (`--seam-stats`:
  `seam.alias.open.<kind>`, by what the two pointers' views say, per leaf
  pair of an ite pointer: `seam.alias.open-pairs.<kind>` counts the pairs,
  and a question is the least decided kind among them) and, later, a
  partial aliasing scenario chosen as a named assumption (A-SEP) when
  aliasing makes a query hard. The open in-tests of reads over
  memory-operation terms (`(j - d) <u n`, SeaM-memops.md) are counted as
  `seam.alias.open-memop.<kind>`.

Equality (`EQ`) refuses `ptr(w)` arguments. Memories indexed by addresses
are `array(ptr(w), bv(c))` (D1): the Select / Store typing rule is the check
that finds an integer at an access position.

## Where the sort is

In every VC (D5), with or without `--seam` (S2e with SeaM, S2g without),
except `--mem=lambda`: that encoding z3-simplifies every written value and
z3 has no pointer sort (`SeaOpSem::ptrSorted`). Under SeaM the deciders
read terms raw (S2a): `sameAddr` (equal or equal by `canon`), views of
nodes and markers, the OFFSET cancellation, ARM's node and marker cases.

## The boundary (erasure)

`SeaOpSem::solverTerm` is the one place where pointers end. The eraser
(`PtrIndexExpander`) maps `ptr(w)` to `bv(w)` (constants, fdecls and array
sorts rebuilt over `bv(w)`, same names), every marker to its argument,
`PTR_DIFF` to `bvsub`, `PTR_EQ` to `EQ`, OFFSET and PTR_INDEX* to
arithmetic. The arithmetic:
- with the rewriter (`--seam` / `--simplify`): SeaM's normalized sum
  (`flat`), so the erased query is the one SeaM's sums gave before the sort;
- without it: the lowering's term, `bvadd(b, ((x_1*s_1 + ...) + (c + K)))`,
  with `x` for a stride-1 index (S2g; `bvmul(x, 1)` before).
An erased object constant `obj.k : bv(w)` is registered as an alias of its
pointer (`BaseRegistry::addAlias`), so consumers after the boundary
(Ackermann's layout values, the eq audit) still see the object. A
pointer-sorted term left after `solverTerm` is the error
`vc.ptr-sort-at-boundary` (checked before Ackermann). Models: a pointer
constant is evaluated through its erased twin; replay reads pointers as
addresses (after placement).

## Measured (vcc-pi2, one data point)

[OBS] The lowered corpus (rev 5 draft 2.2, HEAD 24be323):
18,552 data accesses; fat companions 6,362 (0.34 per data access, all
keyed, none split; 91.5 % at an address that also has a data access);
tracking shadow 590. The keyed layout and the align fix took VC memory
accesses from 33,781 to 24,857 with identical verdicts.

The S2 gates (query-diff against the previous step, the four `--seam`
configs, auto and address mode): S2a identical; S2b identical but 3 z3-equiv;
S2c order / z3-equiv with identical verdicts (`vc-time` 16.6 -> 15.8 s,
`cmp.cancel` 9,340 -> 66); S2d, the cleanup and S2f identical; S2e
order / z3-equiv; S2g identical under `--seam`, z3-equiv without (the
stride-1 term). Fuzz (fuzz-seam ptrmem / any, --seam-check) clean at every
step.

## Decisions

Closed: D1 (memories indexed by `ptr(w)`), D2 (`PTR_FROM_BITS` /
`PTR_TO_BITS` for a pointer's own bits; the split pass emits
`addr.from_bits`), D3 (no exact reprovenance: `from_int(to_int(p))` is not
`p`), D4 (S2 order-only where the term changes), D5 (the sort in every VC,
erased before every backend; lambda mode excepted), D6 (`PTR_EQ`), D7
(`--vc-typecheck` default on), D9 (S1 before S2), D10 / D10a / D12 (keyed
companions, the align fix; the fat cell and its shared antecedent
withdrawn, D14), D15 (`--ptr-mode=auto`), D17 (no A-REPROV).
D13 (`IS_OVERLAP`, first use: the open compare of a ROW step, counted,
decided in the access context, never a `PTR_EQ`;
not yet: the memory-operation range tests, the wide byte fallback,
wider than one unit).
Open: D8 (Lean: a pointer as a structure, S5), D11 (A-PTRINT, built as --ptr-companion=assume with its discharge =check,
off), D16 (the ptrtoint-removal port: an MLIR pass on
the abstract program, only for a corpus that needs it; vcc has none).

## Next (rev 5 stages)

- S4: precision, measured in both modes (pull-ite parity,
  `ptr.load-resolve` through `wselect` and store-map hits, `IS_OVERLAP`).
- S5: Lean (`Ty.ptr`, the erasure lemma, rules restated; seam-lean).
- S6: opaque mode's pointer companion (reconstruction of split pointer
  loads; 207 in 22 vcc jobs). First pull shared conditions out of concatenations. The 116 split loads left on vcc are not ones the companion
  resolves: its guard `PTR_TO_BITS(c) = bits` folds only when the bytes
  already reduce to `PTR_TO_BITS(p)`, which ptr.load-resolve already
  handles. It pays only with A-PTRINT, an assumption. Two shapes remain:
  - hash tables: every byte is `ite(c, byte_k(p), older)`, with c repeated
    inside the branches of a join;
  - reads of an initial memory, and byte stores at an unknown index.
  `--seam-pull-concat=N` (off by default, rule ptr.load-resolve-concat)
  pulls a condition shared by the bytes out of the concat. Each byte is
  taken under c = true / false along its ite spine. A side that
  reassembles a stored pointer is that pointer. The rule is address-exact (the pointer keeps the loaded address).
  A side is split only if each of its bytes that is not an ite is bits of
  a pointer (else no split can reassemble one: the side is a leaf). It is
  split on a condition that every ite byte of it has (in the ites
  reachable through ites, at most 64 per byte) when there is one: one
  split can then reassemble a pointer. Otherwise it is split on the first
  condition of its first ite byte (`ptr.load-resolve-concat.unshared`): a
  condition that only some bytes have reassembles a pointer only after
  further splits, one per guard, within the pull budget N. With
  `ptr.overlap.cancel-num` the bytes' guards are shared. What stays open in byte mode: the byte
  pairs k != k' of a store and a load at unrelated pointers (partial
  overlaps of misaligned pointers), one guard per difference k - k', which
  only an alignment premise would decide.
  On vcc it fires 87 times in 4 jobs (hash_table_eq 72, hash_table_find 9,
  hash_table_init_* 3 + 3), with identical verdicts and unchanged A-INB
  conditions (4842); the resolved pointers are callbacks no access goes
  through.
- S6, as rev 5 2.4 with the companion keyed by the memory
  SSA value in OpSem (SeaM's ite join makes VCGen's join of a companion an
  ite too; no new registers): `--ptr-companion=exact|check|assume` (default
  off).
  - Written at pointer stores (whole, or the offset-0 byte of a split one),
    copied by a memmove (`mcopy` over the companions) and kept by
    memset / memhavoc / integer stores. A stale cell fails the guard.
    Memories with no recorded producer get a fresh array (`ptrc.*`).
  - The split load `from_bits(bits)` at q is
    `ite(to_bits(c) = bits, c, from_bits(bits))`, `c = M.ptr[q]`.
  - Counters: `vc.ptr-companion: loads stores copies fresh` and
    `vc.ptr-companion-arrays` (companion reads the query still has).
  - [OBS] vcc-pi2, exact: 207 loads in 22 jobs (the rev 5 population).
    Verdicts are identical, and from-bits-left.concat stays at 116 with the
    same A-INB conditions, so no load resolves that did not without it. The
    cost: companion arrays reach the solver in 8 jobs (Ackermann reads
    5463 -> 5578), and hash_table_eq's solve time goes from 36.7 to
    61.7 s. As expected: the guard folds only where the bytes already
    resolve.
  - A-PTRINT (rev 4 D11, a named premise: "a pointer loaded from memory is
    the one last stored there"). `=assume` drops the guard: the load is c,
    reported as `seam.ptrint: assumed loads=<n>`, and a sat answer reruns
    with `=exact` (vc.ptrint-sat). `=check` is the discharge job: the exact
    encoding, where every guard is the property and the program's
    assertions are not. Unsat prints `seam.ptrint: holds`, sat prints
    `seam.ptrint: not discharged` (a split load may read bytes that are not
    the companion's pointer, e.g. a cell no pointer was stored to).
    The replay of a check-mode sat keeps a concrete companion (the bits of
    the pointer last stored at each address, built as OpSem builds the
    companion) and checks every split load's guard on the run. Each load
    that fails is printed: `seam.ptrint-violated` (a pointer was stored at
    the address and its bits are not the loaded bits) or
    `seam.ptrint-unstored` (no pointer was stored there, the companion cell
    is fresh), then `seam.ptrint-replay: loads holds violated unstored`.
    The program's assertion is not the property, so the run may halt
    without failing it. The sat is confirmed (exit 0) when some guard
    fails; otherwise it is `vc.replay-no-failure` (exit 2), as for an
    assertion sat whose replay does not fail the assertion.
- `--seam-congruence` (off by default, rule cmp.congruence in decidePair,
  exact, no premise). Two addresses differ when their difference is
  `sum c_k x_k + d` with 2^t dividing every c_k and not d, as in table
  slots read a byte apart (8i + 1 vs 8j). [OBS] vcc-pi2: it fires 40,264
  times in 11 hash jobs; queries are identical (md5) and verdicts are the
  same. The pairs are decided later anyway (by ARM, which runs after it).
  In the unit test (`test/VC/seam/congruence.mlir`) nothing else decides
  them.
- Census of same-object alias questions (D13). An undecided same-base
  compare is reduced by `overlap` to an integer compare of the offsets, so
  there is no pointer left to mark. The W-BYTE-IN guard `(j - i) <u w` is
  the same kind of question, as is a leaf of a ROW compare tree reduced to
  an offset compare (rowCompareEq). All are recorded (`Rules::aliasReduced`), and
  the census counts the normal forms of the recorded terms in the final
  query as `same-object` (they are normalized further after recording; a
  first version matched them as recorded and found almost none). Queries
  are unchanged. [OBS] vcc-pi2: alias.open.all 75,167 in 90
  jobs, of which same-object 65,174 (73 jobs), one-unknown 9,647,
  both-unknown 207 and diff-object 139; `seam.alias.same-object-reduced`
  (asked, decided or not) is 108,293. The open questions on vcc are mostly
  symbolic offsets into one buffer.
- S7, not scheduled: pointer-sorted cells.
- Left from S2: `flat` / `flatDeep` are both the eraser's arithmetic and
  how `view` / `pullIteNode` read a node; the boundary keeps sharing them
  (that is what keeps the `--seam` queries identical) until views read
  nodes structurally only (S4). Done: the unit drivers (SeaMRules,
  StoreMap, MemOps, AddrRange) build pointer-sorted inputs as OpSem does
  and type-check them; an erased twin is an index alias of its object
  (`addAlias`), not a second object.
