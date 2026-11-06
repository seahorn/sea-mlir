# SeaM: the null object (and objects of size 0)

Status: the null-object and size-zero access rules are implemented. The
implementation details and deviations are listed at the end.

## Why

Null is modeled as a special *null object*: an offset from null is an
offset in that object, and the null object compares with other objects by
base. This also covers Rust's dangling pointers (`NonNull::dangling()`) and
pointers into zero-sized types.

Evidence that it matters now:
- array_list_swap (before memops step 1): the dominant undecided ROW
  compares were `tmp + k` against `ite(<null path>, null + s*i + k,
  obj.9 + s*i + k)`; the obj.9 leaf is decided (different bases, A-INB),
  while the null leaf remains undecided.
- After the memory-operation change: hash_iter_begin with z3 went 3.0 s ->
  29.7 s for the same reason: its reads go through a pointer with a null
  alternative, so each read stays one undecided range test per memory
  term (SeaM-memops.md, results).

Today null is the numeral 0 after placement (`sea-place-static` replaces
`sea.ptr.null` by address 0; objects are placed from address 1 or the
text segment start, so 0 is in no object). In the VC, `null + o` is an
*absolute* ARM entry (a number that may be anywhere), and an absolute
entry is never separated from a base (AddrRangeMap.hh:375-420).

## Proposal

1. **Null is an object.** Placement records it like any other object: a
   segment `sea.segment "null" base 0 size 0 align <max> kind "null"`, and
   `sea.ptr.null` becomes that segment's result, not the numeral 0. In the
   VC (with `--seam`) it is the registered base `obj.null`, equal to 0 by
   the layout side condition, as every other base. `null + o` is then an
   offset in the null object: a pointer view / ARM entry with base
   obj.null. Without `--seam`, nothing changes (the segment's value is 0).

2. **What the null object means: size 0.** No access of positive width is
   in bounds of it. A-INB on an access through null is therefore false: a
   null dereference violates A-INB, as any out-of-object access does, and
   the separate bounds/null-check job discharges it (Conway et al.).
   No new assumption is introduced.

3. **Access compares (ROW, cmp.diff-base with diffBase = true).** For two
   *access* addresses, a leaf pair whose one side is in an object of size 0
   (null, or a ZST object) is decided different under A-INB: the access
   cannot be in that object, so the pair is vacuous. Leaf pairs of two
   positive-size objects are decided as today. For an access address
   `ite(c, null + o, obj.9 + o)` against `tmp + k`, every leaf pair is now
   decided, so the read skips. Premise: A-INB of both accesses, as today;
   `inBounds` of a null leaf is false, so the premise of the whole access
   is `!c && inBounds(obj.9 + o)` — satisfiable as long as one leaf lives.
   **Aggregation of definite violations.** Today
   `Rules::inBounds` threads one `violated` flag through every ite leaf and
   `inBoundsLeaf` sets it for any leaf that cannot hold the access
   (SeaMRules.hh:375-419; `rangeInBounds` likewise): registering obj.null
   alone would turn every mixed `ite(c, null, A + i)` into a static
   violation and disable A-INB for the whole run. Change, for all leaves
   (ordinary out-of-bounds leaves and ARM alternatives too, not only null):
   an impossible leaf contributes `false` to the guarded condition; the
   access is a definite violation only if *every* alternative is
   impossible (syntactically established; conservative). Only then the
   static fallback applies (`vc.seam-inb-violated`); the replay witness
   check (`seam.inb-violation`) still reports a null dereference from the
   interpreter when a returned witness has one.

4. **Program compares (pointer equality, diffBase = false) are not
   rewritten by layout.** They stay pure bit-vector reasoning:
   `null == null` (same base, same offset) is decided; `obj.A == obj.null`
   is left to the solver, which gets the layout equations (obj.k = numeral)
   as side conditions. The rewriter's constants are independent without
   those equations, so a placement-based rewrite would require its own recorded layout premise.
   No layout-based rewrite is applied. `null + k` against `obj.x + j` for
   symbolic offsets is never decided by bases: they can be numerically
   equal (offsetof-style arithmetic on null), and non-nullness of
   `obj.A + off` needs offset facts, not only a nonzero base. Pointer
   equality and access disjointness stay distinct.

5. **Other objects of size 0.** Allocations of size 0 (a zero-sized type,
   `malloc(0)` that returns a pointer) are objects of size 0 too. Under the
   tracking contract below every object has its own base, but a pointer
   compare is still not decided by base (see "Shadow memories"). Under rule 3 their *access* pairs are vacuous like null's; their *pointer* compares stay undecided by
   base. Rust's dangling pointer (an aligned non-null address with no
   object) is the same: [OPEN] how it reaches us — from LLVM it is
   `inttoptr(align)`, a numeral; the exporter would need to recognize it,
   or sea-mlir gets an explicit `sea.ptr.dangling <align>` op with its own
   size-0 object per site.

6. **Width and range, not "mentions a size-0 object".**
   The rule is about accesses of positive width. A range operation of
   length 0 accesses nothing, even through null: the range premise
   `n == 0 || (n <=u size && off <=u size - n)` reduces to `n == 0` for a
   size-0 object. So a skip over a null destination range is valid because
   A-INB forces the range empty; a read through null is infeasible because
   it has positive width. n = 0 is preserved in access accounting,
   fallback, source / destination range premises and replay; `mem.empty`
   stays an unconditional simplification.

7. **Provenance boundary.** Null provenance comes only
   from `sea.ptr.null` (and survives the operations that keep pointer
   provenance: ptr.add, joins, pointer-valued store / load through the
   pointer view). An integer 0 cast to a pointer, a pointer loaded from
   zeroed memory without a view, or a small aligned constant is NOT
   reinterpreted as the null object: it stays an absolute value, as today.
   Rust's dangling pointers need an explicit producer contract (an op);
   deferred.

## What changes in code (estimate)

- `sea-place-static`: a null segment (kind "null", base 0, size 0);
  `sea.ptr.null` -> its result. Check: no other object overlaps [0, 1)
  (already true).
- BaseRegistry: register obj.null (kind null, size 0).
- `Rules::inBounds` / `inBoundsLeaf` / `rangeInBounds`: guarded
  aggregation (an impossible leaf is `false` in the condition; a definite
  violation only when every alternative is impossible), for all leaves;
  a leaf in a size-0 object holds no positive-width access.
- Access-compare paths (`decidePair` / `decideEq` with diffBase,
  `distinctObjects` for WOW/smap commutes, `AddrRangeMap::disjoint` with
  crossBase, memops `decideIn` skip): a pair with a size-0 object is
  vacuous-different under A-INB; record it in the Decision as its own
  reason (`cmp.size0-vacuous`, premise A-INB) so the trail and
  `--seam-check` see it.
- Program-compare paths: no change except that null is a base; no
  layout-based rewrite (left to the solver, which has the layout).
- Interpreter: unchanged (null already has its own error).
- Tracking / fat pointers: `sea.ptr.base(null)` = the null object's base
  (0) — consistent with today's value; check the shadow lowering keys.

## Assurance

- `--seam-check`: a firing that uses `cmp.size0-vacuous` is checked as a
  whole rewrite under its complete premise (`inBounds(access_1) &&
  inBounds(access_2)`, the guarded conditions), which must be satisfiable;
  an impossible leaf is neither a reason to reject the firing nor
  independent evidence on its own.
- Unit tests: access pairs with null / size-0 leaves (vacuous under A-INB,
  undecided without it); program compares with null (decided only by the
  layout facts); `null + k` vs objects (undecided); the satisfiable-premise
  check.
- Lit / corpus: reads and copies through `ite(c, null, obj)` pointers, sat
  variants where c can hold (the null dereference makes the sat a
  `seam.inb-violation`, exit 4 under --seam, a null-deref error under
  sea-run), offsetof-style null arithmetic in a program compare.
- Focused acceptance matrix: each with independent
  scalar / range premises through the semantics-only boundary:

  | Case | Expected |
  |---|---|
  | `ite(c, null, A + i)` read vs an access to B | skip under a satisfiable full A-INB premise; no any-leaf fallback |
  | every alternative invalid, positive width | static fallback; the safety job reports the violation |
  | zero-length range through null; symbolic n in {0, 1} | n = 0 admitted; n = 1 violates the range premise |
  | two size-0 objects at one address | pointer equality numeric; positive-width accesses infeasible |
  | `null + k == obj.A + j`, symbolic offsets | not decided by bases |
  | pointer vs null compare with fixed layout | left to the solver |
  | more alternatives than the ite-leaf cap | ARM gives the same conditional answer |
  | null stored through a pointer and loaded back | provenance kept where the view keeps it; integer payloads not reinterpreted |

- The expected result is no unexplained change on the common valid-access
  domain. An old sat whose only failing executions access
  null may become a conditional unsat: intended (the null access is the
  safety job's), recorded per case. Replay diagnoses a violating witness
  when one is returned; it is not the safety check.
- Then remeasure hash_iter_begin (z3 3.0 -> 29.7 s after memops) and
  array_list_swap (z3 still T/O); then vcc-small and the vcc bench.

## Shadow memories (tracking contract)

Tracking on a `malloc(0)` object originally disagreed across the abstract
interpreter, plain VC, and `--seam` replay. `sea-lower-tracking` keyed its shadow by
`sea.ptr.base(p)`, an object of size 0 shared its base with a neighbour
in the SeaHorn layout (so the plain lowering read the neighbour's flag),
and SeaM read the shadow access in a size-0 object as vacuous under A-INB
(so it followed the abstract semantics); the two disagreed. Contract:

1. **Every object gets its own address.** `sea-place-static` reserves at
   least one unit of address space for an object of size 0 in every layout
   (`addressRoom`; the compact layout already did). The object's size stays
   0 (its segment says size 0; no access of positive width is in it; A-INB
   is unchanged); only its address room is >= 1, so no two objects share a
   base (postcondition of the pass). This is C's `malloc(0)`: it returns
   null or a unique pointer. Rust's zero-sized types and dangling pointers
   are not placed objects here (point 7: an explicit producer op, later).
   Program compares are unchanged: still not decided by base (`p == q` for
   two placed objects is left to the solver, which has the layout; with
   distinct bases it is false, as in the abstract interpreter).
2. **Shadow memories are read by object identity, not A-INB.**
   `sea-lower-tracking` starts every shadow memory with
   `sea.mem.const 0 shadow` (a unit attribute on the memory-creating op;
   the interpreter ignores it). `SeaOpSem` registers the start term of each
   shadow `sea.mem.const` in the base registry (before the first block is
   executed, so no term is classified first), and `Rules::shadowMemory`
   says whether a memory term (stores, store-maps, ites, memory-operation
   terms) is built over one. A load or store of a shadow memory:
   - is not a program access: no A-INB condition, no static violation, no
     fallback (`accessAddr`; counter `shadow.access`). Querying or resetting
     metadata is not dereferencing application storage, and an application
     bounds check cannot discharge it;
   - gets no A-INB rule: no `cmp.diff-base`, no `cmp.size0-vacuous`, no
     ARM, no R-SKIP or WOSMAP-COMMUTE, no memops skip by objects;
   - instead: two keys that are two different registered bases at offset
     the numeral 0 whose placed numerals differ are different cells
     (`shadow.distinct-base`, recorded in `seam::Decision`; it decides ROW
     skips, R-ABORT entries, the ite compare trees of ROW, and WOW-COMMUTE
     into allocation order). The premise-free rules (same term, same base
     with numeral offsets, cancellation) apply as everywhere.
   A key is an object base by construction: `sea.ptr.base(p)` becomes
   `p - off(p)` (sea-lower-fat-pointers), which normalizes to `obj.k` (or
   an ite of bases, for a pointer chosen by a select or a join). A key that
   does not normalize to a base (a loaded pointer) is not decided by
   identity: the compare is left to the solver (premise-free).
3. **The premise is LAYOUT, a named fact, not an assumption.** The rule is
   valid given the layout equations `obj.k = <numeral>` (side conditions of
   the VC on every path): after them it compares two different numerals.
   Without them the rewriter's base constants are independent, so the rule
   is not premise-free over plain bit-vectors; it gets its own premise bit
   (`seam::PremLayout`, trail and `--seam-trace` name `LAYOUT`, counter
   `premise.layout`), and `--seam-check` states it as exactly the layout
   equations (`SeaOpSem::firingPremise`). It is never reported as assumed
   (`seam.inb` counts only A-INB), because nothing about the program is
   assumed: placement (1) makes the numerals distinct, and the rule checks
   that they are (two bases with equal numerals, e.g. a hand-placed
   program, stay undecided). Chosen over a premise-free rewrite of the
   bases into their numerals, which would be a layout rewrite of terms
   that a program compare may share (point 4).
4. **Fat pointers need no marking.** `sea-lower-fat-pointers`' companion
   memories (offset, size of a stored pointer) are indexed by the address
   of the data access they shadow (the same address as the pointer store /
   load); they are not keyed by base. They use the `keyed` layout (a cell
   of any width at every address, independent cells, sea-mlir 38db9f6), so
   a companion access is one whole cell and is never split, also when the
   data access is. A-INB is shared, not restated: a companion (keyed)
   access is exempt from its own condition only when a program access of
   the same block has the same address term (a range op: the same
   (d, n)), whose A-INB condition under the same path condition implies
   the 1-unit facts the rules use on the companion memory
   (`SeaOpSem::settleKeyed`, counter `companion.access`). An unmirrored
   one is counted as a program access with its own condition (warning
   `vc.seam-companion-unmirrored`, counter `companion.unmirrored`).
5. **Diagnostic.** With (1) and (2) both in, tracking on an object of size
   0 is exact (the regression `test/VC/tracking/size0-identity.mlir`:
   interpreter, plain lowered VC and `--seam` with replay agree; reset
   through null, through a select of a size-0 and a positive object,
   is_modified of a size-0 object), so no diagnostic is emitted for it.
   What stays visible: a data `sea.mem.const` whose start term equals a
   shadow start (same sort and value) is read as a shadow memory too
   (sound: fewer rules, no assumption added): warning
   `vc.seam-shadow-shared-start`. No corpus program has a data
   `sea.mem.const`.

## Open

1. Size 0 with conditional access reasoning; no reserved null range (it adds a layout
   policy and still does not make arbitrary `null + offset` disjoint).
2. Rust dangling pointers: deferred; an explicit producer op, not
   recognition of `inttoptr(align)` (point 7).
3. Symbolic placement later (parked): the null base stays the constant 0;
   other bases symbolic and nonzero.

## Implementation (rev 2)

Code:
- `sea-place-static` (lib/Transforms/PlaceStatic.cpp): if the program has a
  `sea.ptr.null`, a segment `"null"` (kind `null`, base 0, size 0, align
  2^62) is the first object (order 0) and its result replaces every
  `sea.ptr.null`. A program without `sea.ptr.null` gets no null segment.
  `sea.ptr.from_int 0` stays `sea.addr.from_int 0` (point 7). Without
  `--seam` the VC sees the numeral 0, as before; with `--seam`, OpSem
  registers `obj.null` (size 0) like any segment, with `obj.null = 0` as a
  side condition.
- Guarded aggregation (SeaMRules.hh `inBounds` / `inBoundsLeaf` /
  `armInBounds` / `rangeInBounds`): a leaf returns `false` when it cannot
  hold the access; ites combine with `ite(c, t, t) = t`; the `violated` flag
  is set iff the whole condition is `false`. For all leaves, not only null.
- `cmp.size0-vacuous` (premise A-INB), recorded in `seam::Decision` (so the
  trail, the counters and `--seam-check` see it):
  - `decidePair` with `diffBase`: a side that is a pointer view into a
    size-0 object, after the premise-free same-base decisions (equal terms,
    numeral offsets of one base are still `cmp.same-base` / `cmp.refl`);
  - ARM (`AddrRangeMap::disjoint` with `crossBase`): entries of size-0
    objects are dropped from both maps first (new out flag `usedSize0`; a
    map left bottom is vacuous); `Rules::armDisjoint` records `arm.skip` and
    `cmp.size0-vacuous`;
  - `distinctObjects` (WOW-COMMUTE, WOSMAP-COMMUTE, R-SKIP): two different
    registered objects, one of size 0, are distinct for accesses; counted as
    `cmp.size0-vacuous`;
  - memops `decideInPair`: the read `j` in a size-0 object (positive width),
    or the destination `d` in a size-0 object (A-INB forces `n = 0`: the
    range is empty) is Out; an ARM-only `j` drops its size-0 provenances.
- `rangeInBounds` of a size-0 leaf with a symbolic length is `n = 0`
  (numeral 0: true, before any leaf; numeral > 0: false). `mem.empty` is
  unchanged and premise-free.
- Program compares: unchanged (no layout rewrite, no size-0 rule).

Deviations and additions (none changes the design's contract):
1. ARM leaves (no pointer view): before, any map entry wholly outside its
   object made the access a definite violation. Now a provenance whose
   entry (in the map of the whole leaf) is wholly outside its object is
   `false` in `armInBounds` (every provenance-k leaf has its value in that
   interval, so its range check is false anyway) and the leaf is `false`
   only if every provenance is and no part is absolute. This is the same
   guarded rule applied to ARM alternatives (the design says "ARM
   alternatives too"); `test/VC/seam/arm-unreach.mlir` changed from a
   fallback to `seam.inb: assumed` (same verdict, unsat).
2. A vacuous pair is decided *different*, as the design says; a mixed ite
   whose other leaves are equal (e.g. `ite(c, null, A) = A`) is therefore
   still Unknown. Treating a vacuous pair as a wildcard (either answer) is
   valid under A-INB too and would also give hits; not done.
3. [Superseded by "Shadow memories" above: shadow accesses are no longer
   A-INB accesses, and size-0 objects have their own base.] Tracking
   (sea-lower-tracking) reads and writes its shadow memory at
   `sea.ptr.base(p)`, an access of one cell. For p = null that is an access
   in the null object: `is_modified` / `reset_modified` of a pointer that
   is only null is now a definite violation (static fallback, no soundness
   issue); for a mixed pointer the null alternative is vacuous, which agrees
   with the abstract semantics (null is never modified, and a shadow key is
   a base, never `null + k` with k != 0, so "different from every other
   base" is also numerically true). For a malloc(0) object the shadow key
   collides with its neighbour's in the SeaHorn layout (a pre-existing
   imprecision of the lowering: `is_modified(p0)` reads the neighbour's
   flag); the vacuous rule follows the abstract semantics there, so a
   replay of such a witness could disagree with the placed program
   (exit 2). Not seen in any test or corpus program; a shadow keyed by
   object identity would remove both.
4. `--seam-check` with Yices refuses queries over `sea.mem.const` (a lambda
   term; pre-existing): the acceptance tests use
   `sea.mem.nondet` memories.

Results (Release, timeout 300 s, `--lower --widen-memory=8 --seam`;
compared with the earlier implementation):

| job / solver | before solve (dag) | after solve (dag) |
|---|---|---|
| hash_iter_begin z3 | 32.1 s (15719) | 2.3 s (6500) |
| hash_iter_begin yices | 1.0 s | 1.3 s |
| hash_iter_begin bitwuzla | 44.3 s | 6.1 s |
| array_list_swap yices | 10.0-11.1 s (2370) | 14.0-15.8 s (2327) |
| array_list_swap bitwuzla | 11.2 s | 9.2 s |
| array_list_swap z3 | T/O | T/O |

hash_iter_begin: 5805 `cmp.size0-vacuous`, `row.store-skip` 4204 -> 5800,
every mset / mcopy read now skips (were 299 undecided range tests).
array_list_swap: 4 of 126 undecided mcopy reads now skip; z3 still times
out; Yices is slower on the slightly smaller query (3 runs each).
vcc-small: no verdict
change, no fallback; z3 35.6 -> 11.5 s total (hash_iter_begin 29.7 ->
2.6), Yices 9.9 -> 13.6 s, all of it ring_buffer_release2 (7.1 -> 10.9 s
Yices, 4.0 -> 7.0 s z3), which uses no size-0 rule: its only change is that
null is `obj.null` (side condition `obj.null = 0`) instead of the numeral 0
in its program compares (15 more DAG nodes, a few more `cmp.same-base` /
`cmp.ite-push`). [HYP] A solver-side effect of the term shape; folding
`obj.null` to 0 would be a layout rewrite of program compares, which this
patch does not do (point 4).

Verdicts: test/VC/seam and test/VC/memops (every program, z3 and Yices,
plain and `--seam`): no verdict change; one fallback removed
(arm-unreach.mlir, deviation 1). `--seam-check` over every program there:
the same counts as in the earlier run (invalid = unknown = unexpressible = 0
wherever it runs; Yices aborts on `sea.mem.const` lambdas, deviation 4, as
before). The acceptance matrix is test/VC/seam/null-*.mlir; its one
intended verdict change is null-size0-shared.mlir @access (plain sat, its
only failing executions store through a malloc(0) object: conditional
unsat under `--seam`).
