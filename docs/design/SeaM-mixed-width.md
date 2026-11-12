# SeaM: mixed-width accesses in one byte memory

Status: byte-memory width terms are implemented behind `--seam-wide`. This
document describes their contracts and the aligned word-level proposal.
Implementation order: semantics-only wide terms and span accounting; then
W-HIT / W-SUB / W-SKIP; old byte and map paths guarded before mixed maps.

The design goal is to promote accesses to wider memory operations where
possible, while allowing narrow and wide operations to share one memory.

## Why

Two ways to avoid byte-granular memory today:

1. Per memory: `sea-widen-memory` (`sea-vc --widen-memory=8`) gives a byte
   memory 8-byte cells when every access to it is a whole aligned word
   (lib/Transforms/WidenMemory.cpp). SeaHorn has one word size per job,
   chosen by hand (`--horn-bv2-word-size`, per-job yaml in
   verify-c-common); ours is chosen per memory, by use. It changes the
   array's cell sort, so it also saves at the solver.
2. Not done: per access. A memory with one narrow access stays bytes, and
   every wide access to it pays bytes: `sea-split-wide-access` turns a
   64-bit load into 8 one-byte loads and a concat (and a store into 8
   stores), before the VC.

Widening census (vcc, 228 jobs, `--widen-memory=8 --widen-memory-report`): 1350 memories widened, 531 kept as bytes; of these, 434 (82%)
only because some load or store is narrower than a word (231 stores, 203
loads); the rest non-constant range lengths (51) and unproven alignment
(46). 187 of 228 jobs have at least one byte memory. The census shows the
opportunity to avoid eager intermediate byte work; it does not show how
many wide reads survive to the solver boundary (where they are bytes
again, below): the gain is a hypothesis to measure.

## Scope

- Byte memories under `--seam` only. Word memories (widened, aligned
  cells) keep the whole-cell path; other layouts keep
  `sea-split-wide-access` for multi-cell accesses. The pass gets a mode
  "leave byte-memory accesses whole" (used by the `--seam` lowering), not a
  global skip; its postcondition and the VC input validator say which mode
  accepts multi-cell accesses on byte memories.
- Whole-memory widening stays first (cheaper: it changes the cell sort);
  mixed-width terms are for the memories it refuses.
- Pointer values: the placement / fat-pointer contract is unchanged
  (Passes.cpp lowers tracking and fat pointers and places before widening
  and splitting; after placement a pointer value is a bit-vector, split
  today without error). The gain for them is avoiding byte work on that
  bit-vector, nothing more. No abstract-pointer model, no endianness mode,
  no width-tagged lambdas.

## Terms and their meaning

- `wstore(M, a, v)`: a write of w = bits(v) / 8 consecutive bytes;
  `wselect(M, a, r)`: a read of r bytes. w = 1 and r = 1 are the existing
  store / select.
- Meaning (the definition; the semantics-only boundary
  `SeaOpSem::solverTerm` expands by it, no solver sees a w-term): little
  endian, as SplitWideAccess.cpp and the interpreter:
  `wstore(M, a, v)` = byte stores of `extract(v, 8k, 8)` at `a + k`, k = 0
  .. w-1 (low-offset, bit-count notation: bits [8k, 8k+8) of v);
  `wselect(M, a, r)` = concat of the bytes at `a + k`, k = r-1 .. 0.
  Addresses are bit-vectors of the index width, arithmetic modulo 2^n.
- Supported width domain: 0 < w, r <= 2^n (n = index bits): one access does
  not revisit an address. Outside it (only possible for tiny address
  widths: with 2-bit addresses a 5-byte store at 0 rewrites address 0)
  the w-rules do not apply and the access is expanded to bytes. The
  verifier (SeaOps.cpp:241-248 checks only divisibility by the cell) gets
  the bound for byte memories in this mode.

## Rules (a read [j, r) over a write [i, w))

Intervals are circular (modulo 2^n), exactly as the memops membership
(SeaM-memops.md: common-base cancellation is modular; linear order is a
separate question). With k = j - i (mod 2^n):

- W-HIT: k = 0 and r = w: the value v. Holds for unaligned addresses too.
- W-SUB (containment): k + r <= w as unsigned integers (k <u w and the read
  ends inside): `extract(v, 8k, 8r)`.
- W-SKIP (disjoint): the circular intervals do not meet: k >= w (as
  unsigned) and (i - j) mod 2^n >= r. Decided by numerals of one base (the
  difference is a numeral after cancellation), or across objects under
  A-INB (cmp.diff-base: the whole spans are in their objects), or by ARM
  with circular intervals (or mathematical endpoints that are proved not
  to wrap, premise stated). A linear shortcut is used only with that
  premise stated.
- W-SPLIT (partial overlap, or a wider read over a narrower write): the
  read is assembled from pieces: the overlap from v (an extract), the rest
  read below the write; the only fragmentation, per pair.
- Undecided: the w-read stays a term over the write (no ite in the first
  version: address inequality is not disjointness for unequal widths); the
  boundary expands it to bytes. An optimized ite needs overlap and
  containment guards and an exact partial-overlap arm: later, if measured
  worth it.

Range operations (mset / mcopy / mhavoc terms) over a w-read: the
membership test becomes the circular interval test of [j, r) against
[d, n): inside -> hit, disjoint -> skip, else split. The hit forms keep the
existing versions: an mcopy hit reads `wselect(Ms, s + (j - d), r)` from
its explicit source memory version; an mset hit is the repeated byte; an
mhavoc hit `wselect(H, j, r)` from the one H of that op. A straddling read
assembles its parts from the right versions: pieces never become
sequential reads of the updated destination nor fresh havoc values.
Source and destination ranges keep their own premises.

## A-INB and the checker: per-access widths

Today SeaOpSem passes memory cell units to `accessAddr` and the trail
context (OpSem.cpp `unitsOf`, memLoad / memStore), and `accessPremise`
states `inBounds(a, units)` for every address of a firing (SeaMRules.hh
`accessAddresses`: the read, the store below it, store-map entries): right
for single-cell accesses, wrong for mixed widths (a narrow read's width
would become the premise of a wide writer, weakening the obligation; a wide
read's width would overconstrain a narrow writer, making valid firings
look invalid or vacuous).

- Each access carries its byte span (address, width): the load / store's
  own, each writer's own (a store below a read, each store-map fragment),
  attached to translated reads and fragments; ranges as today.
- A-INB is "every whole span stays in its object"; `accessPremise` states
  inBounds per (address, width) pair it collects; shadow memories keep
  identity treatment (no data-memory A-INB).
- A-INB is assumed and reported, not discharged: the functional run relies
  on it (conditional soundness). The separate bounds-check instrumentation
  to discharge whole spans is not built yet. `--seam-check` checks rewrite
  obligations; it does not discharge A-INB.
- This accounting is part of the first implementation step, with the
  terms.

## Store-Map: last writer wins

StoreMap.hh relies on distinct numeral offsets being independent cells
(writes at distinct offsets commute; `offset -> latest value`; expansion
oldest to newest). With widths, distinct keys may overlap, at equal widths
too ((0, 8) and (4, 8)).

- First version: a wide write that overlaps an entry of the map (any width)
  ends the map run: the map is closed and the write stays a `wstore` above
  it. Non-overlapping fragments only inside one map; an exact-key hit is
  then safe (no newer overlapping writer exists inside the map).
- Later, if measured worth it: a canonical set of non-overlapping live
  fragments with insertion slicing (retire / split overwritten fragments).
- Expansion keeps the chronological meaning, independent of lookup.
- Tests: unequal widths at the same start; shifted equal widths; a narrow
  overwrite of a wide value; restoration by a later wide write; old
  memory versions kept across a fork.

## Assurance

- Each rule is an identity over the byte expansion within the width
  domain (W-HIT, W-SUB, W-SPLIT exact; W-SKIP exact by circular numerals of
  one base, A-INB across objects), checked by `--seam-check` through the
  boundary, with the per-access-width premises.
- An independent small-address byte-reference oracle (memops-equiv style):
  every interval relation (inside, disjoint, partial, wrap-around), little
  endian with non-uniform bytes, symbolic addresses and memory-dependent
  indices, copy overlap and distinct source versions, repeated reads of one
  havoc, store-map history forks, mixed byte / word memories, the boundary
  with row expansion off or a low rewrite budget; checker premises and
  model / replay calls, not only the final query. Compare byte-expanded and
  wide encodings under the same assumptions and symbolic-length domain.
- Semantic mutants each killed by a named oracle case: reversed byte order;
  overlap replaced by address equality; a dropped newer narrow overwrite;
  mcopy reading the updated destination; the reader's width used for the
  writer's premise.
- Caches: MemOps.hh memoizes reads by (memory, index); a stage caching a
  wide read keys by (memory, index, width) or expands first.
- SAT replay: wide scalar values and the bytes of initial / havoc arrays
  validated consistently (Replay passes scalar values through solverTerm).

## Measure

Not only time: wide reads eliminated by the rules; wide reads surviving to
the boundary (expanded there: bytes paid at unresolved base reads, e.g. an
unconstrained initial byte array); fragmentation (W-SPLIT count); final
solver selects and Ackermann pairs; DAG size; per-solver time. Structural
tests: a long history of provably disjoint wide stores then one wide hit
(no byte work across it); a wide read of unconstrained initial memory
(acknowledged byte fallback). Corpus: vcc with `--widen-memory=8 --seam`,
hash_iter_begin, hash_table_eq and the jobs whose byte memories hold the
most wide accesses first.

## Additional contracts

1. Store-Map: every path is width-aware, the old byte paths included:
   creation (SMAP-NEW), insertion, lookup, miss and store collapse. A map
   miss needs the whole read span to miss every entry (a byte read at 3
   must see a wide entry over [0, 8): today's exact-offset `find` plus
   R-MISS is wrong there); a same-start collapse needs the newer write to
   cover the older span (a newer narrow write keeps the older tail); ANY
   write (not only a wide one) that overlaps an entry closes the run.
   Until interval lookup exists, such reads stay terms (or the map is
   expanded into chronological wide stores). Tests: a byte read inside a
   wide entry; a wide read over two disjoint entries and a hole; a byte
   overwrite inside a wide entry; a newer narrow same-start write keeping
   the older tail.
2. Circular splitting in read-byte order: W-SUB handles contiguous slices
   of the written value; other known relations split at both modular
   membership boundaries and at the wrap of the value-byte offset, or stay
   terms. Pieces are assembled in the read's byte order, one extract per
   covered piece. Examples for the oracle (16 addresses): write [0, 12),
   read [8, 12) gives v[8..12), old[12..16), v[0..4); a full-circle write
   [0, 16) and a read [14, 4) gives v[14..16), v[0..2) (fully covered, yet
   not k + r <= w). k + r, widths and comparisons with 2^n are computed on
   mathematical integers (an n-bit value cannot hold 2^n).
3. Range operations: an undecided relation keeps the term (no statically
   sized split of a symbolic length); a zero length is no change (an empty
   range is disjoint even when it starts at the read; the positive-width
   W-SKIP formula is not reused for n = 0); a symbolic length with known
   coverage may still hit / skip.
4. Width domain: the general dialect verifier is unchanged (positive cell
   multiples); the lowering expands accesses outside the w-rule domain to
   bytes before building width terms (SplitWideAccess's byte expansion),
   and the internal term / VC validation enforces the bound. No source
   access is rejected for it.

## Implementation (increments 1-2; flag `sea-vc --seam --seam-wide`, default off)

- Terms: `W_STORE` / `W_SELECT` (ExprOpArray.hh; wselect carries its result
  sort); `WideExpander` (MemOps.hh) at the boundary, before the memop
  translation. `sea-split-wide-access{keep-byte-wide}` leaves wide accesses
  of exact byte memories whole when 0 < bytes <= 2^n; SeaOpSem builds the
  terms and states A-INB on the whole span (`accessAddr(…, bytes)`, trail
  context units = bytes).
- Span accounting: `Rules::accessSpans` (replaces accessAddresses): each
  address with its own width (a wselect's bytes, a wstore's bytes, a select
  / store's cell bytes at most the context's units; a byte read made by the
  fallback inside a wide load's context is one byte: found by fuzz-seam's
  --seam-check, 4 unexpressible firings before the fix).
- Rules: W-HIT, W-SUB, W-SKIP, W-SPLICE (`wideRelation`: circular
  numerals of one base; across objects under A-INB with single pointer
  views), a wide read through an ite, over a store-map, over a memop term
  (inside -> the hit form of the source version / the repeated byte / H;
  disjoint or n = 0 -> below; across objects -> below), per leaf pair of
  an ite address (W-ITE, below).
- Deviation from rev 3 ("undecided keeps the term"), measured: a width term
  left to the boundary is expanded there over the whole remaining chain,
  with no SeaM rule on the bytes (vcc: hash_iter_* 0.7 -> 27 s with Yices,
  z3 7 T/O). Now an undecided wide read is split into byte reads at that
  point (`wide.split-read`) and an undecided byte read over a wstore reads
  its byte stores (`wide.expand-write`): exact; as an encoding it is no
  worse than byte splitting (the same byte reads at worst), which is not a
  runtime bound. Wide reads of an initial memory or a const array stay terms
  (the boundary makes the same byte reads). Store-Maps never hold wide
  writes in this increment (a wide write is not inserted; byte rules stop
  at a wstore), so amendment 1 holds trivially.
- Tests: sea-memops-test `wide` (hit, sub, little-endian bytes, wrap at
  offset 15, numeral skip without premise, partial overlap -> fallback,
  across objects under span premises and invalid without the writer's span,
  no A-INB -> fallback, mcopy hit, zero-length range, symbolic length ->
  fallback; each checked against the byte expansion), test/VC/seam/
  wide-access.test (inb-ptrmem / inb-ptrmemmove keep their A-INB unsat:
  W-HIT and memop hit), fuzz-seam variants seam-wide / seam-wide-inb0 (all
  modes, --seam-check: no invalid / unexpressible firing).

- Core circular, span, memop, and fallback paths pass 65,536 circular
  cases. Two fixes follow. (1) The width domain is checked
  at the VC entry too (`inWideByteDomain`, WidthDomain.h, shared with the
  lowering): an oversized access without --lower gave a false UNSAT; it is
  now refused (vc.wide-access), and the lowering's offset helper wraps
  offsets mod 2^n (it asserted). (2) WideExpander pins its cache keys.
  Regression tests: test/VC/seam/wide-domain.test and memops wide regressions
  (cache reuse, adjacent-object writer-width mutant, full circle, two-piece
  overlap).

## Proposal: aligned word-level read-over-write and A-ALIGN

Measured (vcc 228, --widen-memory=8 --seam --seam-wide): width
terms do not shrink the VC (summed DAG 1,012,465 -> 1,017,302; Ackermann
reads 5575 -> 5792; of 66 jobs where the rules fire, 7 smaller by 1-3%, 30
larger, e.g. string_new_from_string 1530 -> 2625). Where a pair is decided
the byte rules decide each byte the same way (same VC); the byte price is
in the undecided pairs, and the fallback pays it again. Census of the
fallback: equal-width wide read over wide write undecided 1060
(hash_table_eq 365, linked_list_swap_contents 100, hash_iter_* 40-73),
unequal widths 75, wide read over a byte store 42; byte reads expanding a
wstore 61,382 (linked_list_swap_contents 20,770, hash_table_eq 11,590),
many of them the byte reads of those splits.

Rule W-ROW-ALIGNED: a read of w bytes at j over a write of w bytes at i,
both addresses w-aligned (w a power of two; i, j multiples of w):
    wselect(wstore(M, i, v), j, S)  ~>  ite(i = j, v, wselect(M, j, S))
Exact given the alignment: two aligned w-spans coincide or are disjoint.
One word compare instead of eight byte compares, and the read stays wide
down the chain (it meets the next wstore, a skip, or a hit). The compare is
ROW's (`Rules::rowEqWidth`, W-ROW-CONGRUENT too): decided first with the
access context (decideEq with A-INB across objects, ARM, size 0), and when
undecided built as ROW's compare tree over the ite leaves (rowCompare), so
only the leaves that stay open are alias questions. An earlier version
emitted `IS_OVERLAP(i, 1, j, 1)` and left it to the program compare
(no A-INB, no ARM): a read at `ite(c, A + 24, A)` over a store at B stayed
open (test/VC/seam/wide-row-context.mlir).

Rule W-ITE (`Rules::rowWideLeaves`): a read over a write whose address
(either one) is an ite, at most maxDecideLeaves leaf pairs. Every pair is
decided on its own (wideRelation, then W-ROW-CONGRUENT / -ALIGNED's compare
with the access context) and the read is the ite tree of the leaf values
under the ite conditions: Hit `v`, Sub(k) the bytes [k, k + r) of v, Skip
the read below at j. A read at `ite(c, p, q)` over writes at p and q is
`ite(c, v_p, v_q)`, a word, where the byte fallback gave the concat of
byte-wise ites (a loaded pointer became `from_bits(concat(...))` and every
access through it a new alias question). Default: only when every pair
is decided; `--seam-wide-ite-open` also rewrites an undecided pair, at a
leaf of the read, to the read at that leaf address (open, untuned: on
string_eq_byte_cursor (vcc, A-INB off) it doubled the byte-store
expansions, 42 -> 102). Tests: test/VC/seam/wide-ite-leaves.mlir.

W-SPLICE (Super in `wideRelation`): a write of w bytes inside a read of r
bytes (one base, numeral offsets, the write at byte k of the read, k + w <=
r) is the read below with the written bytes in place, `concat(below[8r-1 :
8(k+w)], v, below[8k-1 : 0])`, exact: a byte store under an 8-byte load
keeps the word (counter `wide.splice`). Also a leaf value of W-ITE
(`wide.ite-leaf-splice`). Tests: test/VC/seam/wide-splice.mlir.

The other wide reads are decided per leaf the same way (`Rules::leafTree`:
the ite tree over the leaf pairs, at most maxDecideLeaves, every pair
decided or no rewrite):
- over a store-map of b (`rowWideSmap`): a leaf at a numeral offset of b
  reads the entries it covers and the word below for the rest (no entry:
  the word below), exact; a leaf in another object is Out by the access
  context (decideInPair without a length: cmp.diff-base, arm.skip,
  cmp.size0-vacuous; A-INB), the word below. Before, only a single view
  into another object was decided, every other read split into bytes, a
  numeral offset of b included. Counters: one pair `wide.smap-hit`,
  `wide.smap-miss`, `wide.smap-skip`; an ite `wide.smap-split` with
  `wide.smap-leaf-hit`, `-leaf-miss`, `-leaf-skip`. Tests:
  test/VC/seam/wide-smap-leaves.mlir.
- over a memop term (`rowWideMemOp`): the in-test of the read span (the
  memops' decideInPair with span r: In when every byte is in [d, d + n),
  Out when none is), per leaf pair of j and d: In the hit form, Out the read
  below. Before: single views, numeral lengths only. Counters: one pair
  `wide.memop-hit`, `wide.memop-skip`; an ite `wide.memop-split` with
  `wide.memop-leaf-hit`, `-leaf-skip`; a length of 0 `wide.memop-empty`
  (reason `mem.empty`). Tests: test/VC/seam/wide-memop-leaves.mlir.

R-LEAVES (`Rules::leafSplit`, byte and wide reads, `--seam-leaf-split`,
default on): a read at an ite address is the ite of the reads at its
leaves, `read(M, ite(c, a1, a2)) ~> ite(c, read(M, a1), read(M, a2))`
(exact, no premise), only when every leaf read is decided down the whole
memory (`leafWalk`: each write met is decided by the rule the leaf read
meets there, down to a write that gives the read or to a memory without
writes), a write gives every leaf (on every side of an ite memory), and
some leaf is not decided apart from the top write. The read of W-ITE and
of ROW keeps j below a write it partly decides: a read at `ite(e, S + 0, ite(e', S + 8, S + 24))` over
stores at S + 0, S + 8 and S + 24 left a read of the initial memory at the
whole ite, under guards that together cover every leaf, and a pointer
loaded there was `from_bits(concat(byte ites))` with every access through
it an alias question. A split pointer load whose bytes then share one ite
spine of split-address conditions is the ite of the stored pointers
(`ptr.load-resolve-leaves`, all leaves or nothing; other shared
conditions, a join's, stay `--seam-pull-concat`'s). The general split
over an ite index (SeaM-memops.md) stays dropped: no split while a leaf stays open. Counters `row.leaf-split`,
`row.leaf-split-leaves`, `row.leaf-split-refused` (a leaf reaches the
memory without writes; Open item 6; it counts refused attempts, one per
byte read and memory level: a read refused at an ite memory can still
split lower down, at a side where a write gives every leaf). Tests: test/VC/seam/row-leaf-split.mlir.

Alignment, two sources:
1. Proved: the linear form of the address: a registered object base with
   alignment >= w (the registry has `align`), plus a numeral multiple of
   w, plus terms with coefficients multiple of w (`8 * i`). No assumption.
2. Promised: the access's `align` attribute (LLVM's promise; nearly every
   access in vcc has `align 8`: hash_table_eq 263 of 267). A named
   assumption A-ALIGN: "every access with align a has an address that is
   a multiple of a". The width terms carry the access's promised alignment
   (an extra argument); a rule that uses it records the premise bit
   ALIGN; the firing premise adds `a mod align = 0` per access (so
   --seam-check validates it); counted (`premise.a-align`) and reported
   like A-INB. Discharged by a separate check: an alignment check in the
   bounds-check instrumentation job (the interpreter already warns,
   interp.align-promise); never by verifying without it.

Note: sea-widen-memory already trusts `align >= k` as proof of alignment
(WidenMemory.cpp `aligned`): an unnamed assumption in today's best mode.
It becomes A-ALIGN too: the pass records which widened classes relied on a
promise (vs. proved structurally), sea-vc reports A-ALIGN when one did.

Byte reads over an aligned word write keep the fallback (W-SUB when the
offset is known, else expanding that write).

Tests: the aligned rule against the byte expansion with the alignment
premise and invalid without it (a misaligned i, j: partial overlap);
proved vs promised alignment; A-ALIGN reported; widened memories that
relied on a promise reported. Measure: the census counters, DAG / Ackermann
reads per job, solve time; hash_table_eq, linked_list_swap_contents,
hash_iter_*.

## Alignment amendments

The identity is confirmed for power-of-two w <= 2^n. A-ALIGN remains a
separate named premise.

1. Congruent starts suffice, exact, no premise: equal widths w (a power of
   two, w <= 2^n) and `(i - j) mod w = 0` (the normalized difference has
   numeral part and every coefficient divisible by w: same base `b + 8x` vs
   `b + 8y`): the w-blocks of i and j coincide or are disjoint, so
   `wselect(wstore(M, i, v), j)` ~> `ite(i = j, v, wselect(M, j))`. Rule
   W-ROW-CONGRUENT. Implemented first (no assumption, no LAYOUT).
2. Proved absolute alignment uses the actual base, not the registry's
   `align` metadata (a placed segment can claim align 8 at base 1;
   SegmentOp::verify does not check it): the registered base numeral is
   divisible by w (as WidenMemoryPass::provenAligned); the rule then records
   PremLayout (the base equation obj.k = numeral) and the firing premise
   includes it. Pure identities (`8 * x`) need nothing.
3. Promise transfer (A-ALIGN), v1 conservative: a read moved below a write
   or through an ite keeps its own promise (same address); every read at a
   changed address (a byte fragment at j + k, an mcopy source read at
   s + (j - d)) has alignment unknown unless structurally provable
   (counterexample otherwise: a 4-aligned destination read translates to a
   source read at offset 1 that partially overlaps a write at 0); a havoc
   hit keeps j and its promise. A rebuilt term carries its own access's
   promise, never the context's or the writer's.
4. Premises compose: the firing premise is the conjunction of the LAYOUT,
   A-INB and A-ALIGN conditions the rule used (not one exclusive branch);
   each address's alignment condition comes from its own width-term
   descriptor.
5. Discharge before erasure: the alignment job checks each original access
   address against its promise, inserted before the access and kept across
   lowering (SplitWideAccess drops `align` on byte fragments; widening
   relies on promises to change the representation), so the check cannot
   assume away the violation it detects. Widening's reliance on a promise is
   recorded durably in the IR (an attribute on the widened memory's
   defining op, surviving sea-opt output), and the widen pass tries a
   structural proof before accepting the attribute (today `aligned()`
   accepts the attribute first), so reliance is reported accurately.
6. Width-domain guard and power-of-two w are explicit preconditions of every
   aligned / congruent rule.

Order: (1) W-ROW-CONGRUENT, measure; (2) structural absolute alignment with
the actual base (+ LAYOUT premise); (3) A-ALIGN promises with transfer,
premise composition, the alignment check in the bounds job, widening
reliance recorded. Tests should cover (positive / negative mathematical
cases, misleading segment metadata, symbolic base needing LAYOUT, different
read / write promises, unaligned memcopy source, byte fragments at nonzero
offsets, width 2^n, entry without --lower, a wrong promise caught by the
alignment job, serialized widened IR reporting).

## W-ROW-CONGRUENT: implemented and measured

`Rules::congruentStarts` (Ackermann.hh LinearForms: the difference of the
two linear forms has numeral part and coefficients divisible by w);
`wide.row-congruent`. Tests: memops `wide-congruent` (exact without
premises, not-congruent fallback, a chain of congruent stores stays word
level). vcc (w8, Yices query): fired 859 times; undecided equal-width pairs
1060 -> 741, split reads 1242 -> 923, byte reads expanding a wstore 61,382
-> 53,814; summed DAG 1,017,302 -> 1,019,242 (base without width terms
1,012,465): no VC gain yet.

Where the rest is (linked_list_swap_contents, 20,770 expansions): 8-byte
pointer loads with `align 8` through pointers that were themselves loaded
(`node->next`, linked_list_helper.c:113, 108; linked_list.inl:57, 66):
opaque atoms, no linear relation, so neither W-HIT / W-SKIP nor congruence
applies; the read is split and its bytes meet word writes they cannot
decide. The memory is byte-wide for the same reason (one store through a
loaded pointer, linked_list.inl:360, not provably aligned). Only the
`align` promises say these accesses are aligned: the remaining gain is
step (3), A-ALIGN.

## Trust boundary, aligned rule, byte membership

- `align` annotations are trusted front-end facts, not an assumption to
  discharge. The premise bit ALIGN is a fact like LAYOUT: never reported as assumed;
  --seam-check adds `address & (a - 1) = 0` for each annotated access of a
  firing. `sea-vc --check-align` (sea-check-align + sea-merge-asserts) is a
  paranoid-mode sanity check of the front end, not part of verification:
  vcc 228 / 228 unsat (every annotation holds).
- Alignment flowing through memory: if every pointer stored into a
  memory is k-aligned, every dereferenced pointer loaded from it is
  k-aligned; pointers read from initial memory have no metadata and are
  never dereferenced (a dereference of one is a bounds violation), so the
  modular invariant needs nothing about initial memory. Planned as an
  inference pass that adds `align` (not yet: vcc's hot loads already carry
  `align 8`).
- W-ROW-ALIGNED implemented (both accesses annotated >= w; the width terms
  carry the access's own annotation; a changed address, e.g. an mcopy
  source read, gets 1): 855 firings on vcc.
- W-BYTE-IN (a byte read at j over a w-byte write at i: ite((j - i) <u w,
  byte (j - i) of v, below), exact, no premise; the byte is selected among
  constant extracts by the offset, not shifted out (use constant extracts rather than shifts)): applied everywhere it
  grew the vcc VC (1,016,660 -> 1,060,409: an ite over objects or two
  objects is decided per leaf pair by the byte rules after expanding the
  write; one membership ite is not); now only for single views into one
  object at symbolic offsets (18 firings). The 46,748 byte reads that
  expand a wstore are decided afterwards by the byte rules: not a cost.

Result on vcc (w8, Yices query): summed DAG 1,012,465 (no width terms) ->
1,016,562; Ackermann reads 5575 -> 5888. Where the byte price is: wide
reads of initial memory expanded at the boundary (445 wselects, each 8 byte
reads: about 3500 of the 5888 Ackermann reads). [HYP] the next gain is a
word-level treatment of initial-memory reads (a packed view of a region
read only as aligned words; a separate design).

## Open

1. How often unaligned wide accesses occur (the census's 46 "not provably
   aligned"); W-HIT covers same-start same-width ones.
2. An optimized ite for undecided pairs (guards + partial arm) vs keeping
   the term.
3. Canonical non-overlapping store-map fragments (later).
4. W-ITE with undecided leaf pairs (`--seam-wide-ite-open`, off): the read at
   the leaf address vs the byte fallback; untuned (string_eq_byte_cursor:
   byte-store expansions 42 -> 102).
5. Memory of wide reads at a whole ite address. On the stellar txqueue
   round-0 program (VC only, `--seam-wide --widen-memory=4`), the per-leaf
   wide reads (store-map, memop, W-SPLICE) raised the maximum RSS from 3.18
   to 4.41 GB. [HYP] A read below a write at the whole address j (W-ITE's
   Skip leaf, the map's and the memop's Out leaves) survives to the
   boundary, and WideExpander keeps its byte expansion. R-LEAVES removes most
   of these reads there: 0.32 GB, DAG 1.94M -> 129k nodes, Ackermann pairs
   159330 -> 6555. With `--seam-words=auto` the initial memory becomes
   words only after R-LEAVES (words.chosen 0 -> 1, Ackermann pairs 6555 ->
   210). Not measured: which surviving read the expansion was pinned for.
6. R-LEAVES guard. The split pays when it removes the read of the memory
   without writes; a leaf that still reaches it becomes a read of its own
   next to the reads at the whole ite address that other reads (over
   memories where no leaf is given) keep. The first guard asked a
   write to give some leaf. On vcc (`--seam --widen-memory=8`, VC only,
   compared with `--seam-leaf-split=0`) it fired in 18 jobs and made
   Ackermann pairs grow in 13 (hash_table_init_bounded 1596 -> 6328,
   hash_table_init_unbounded 1540 -> 6216, linked_list_swap_contents
   148 -> 457), with alias.open unchanged. Allowing one such leaf per split
   still grew 12 of them (hash_table_init_bounded: one leaf of two reaches
   the initial memory, and the read at it is new). The guard now asks a
   write to give every leaf (decided, no option): no vcc job is worse in
   Ackermann pairs, alias.open or query size (3 jobs still split;
   summed query 25,897,860 -> 25,897,553 bytes); stellar round0 (VC only,
   byte / `--seam-wide --widen-memory=4`) keeps alias.open.all 53195 /
   53427, Ackermann pairs 6441 -> 4753 / 6555 -> 4851, VC time 6.5 / 4.7 s
   (6.4 / 4.8 s before).
   Not done: a partial split that keeps the leaves reaching the initial
   memory under one read at their sub-ite (it pays only when no other read
   keeps the whole address).
