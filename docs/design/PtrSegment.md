# Object identity as a pointer constructor: `PTR_SEGMENT(k, a)`

Status: steps 1 and 2 of the migration are implemented: the term, erased
to its twin `obj.<k>`, with `obj.<k> = a` stated at the boundary
(`SeaOpSem::withLayoutFacts`), and the fold's narrowed rule. Step 3 is
deferred. Open question 1 is settled (the per-leaf invariant below). Line
numbers below are of the commit this note was written on.

## The problem

Under `--seam` a placed object is the pointer constant `obj.k : ptr(w)`
(`op::ptr::ptrConst`, `include/sea/Expr/ExprOpBv.hh:129`). Its address is
a separate fact, the layout side condition `ADDR(obj.k) = <numeral>`
(`lib/VC/OpSem.cpp:832-833`). Two things that belong to one object live
in two places:

- identity is syntactic: `view()` (`include/sea/Expr/SeaMRules.hh:1240`)
  finds `obj.k` at the root of a term, and the base registry knows it;
- the address is a variable that a top-level conjunct pins.

A rewrite that substitutes top-level facts can therefore replace an
address by a numeral and change what a term's view says.
`--vc-fold-top-eqs` (FoldTopEqs.md) did this. The memmove read equation
of `test/VC/fold-top-eqs/layout-facts.mlir` reads at

    obj.5 + (ADDR(ite(c, obj.4 + 4, obj.5)) - ADDR(obj.4))

which has no view (its offset mentions two bases). With the layout facts
substituted and the ite pushed, one leaf became `obj.5 + 0xffffffc0`, a
pointer into `obj.5` with a view the program never gave it, and
`cmp.diff-base` decided its alias question under A-SEP in the fold's
round, outside any access, where `--seam-check` cannot state a premise.
The interim fix skips every fact from which a pointer operator is
reachable outside a read (`TopEqFolder::addressDerived`,
`lib/VC/FoldTopEqs.cpp:335`; FoldTopEqs.md, "Pointer observations").

The fix proposed here puts the address inside the object's term, so no
separate fact exists to substitute before the solver boundary.

## The term

    PTR_SEGMENT(k, a) : ptr(w)

- `k`: the segment's name, a string terminal (`mkTerm<std::string>`), the
  name of the `sea.segment` op (`stack.3`, `global.g`, `null`, ...);
- `a`: the placed base, a bit-vector numeral of width w (`bv(w)`); the
  sort is `ptr(w)` with w the numeral's width;
- arity 2. A new `PtrOp` in `include/sea/Expr/ExprOpPtr.hh` (name
  `"ptr-segment"`) with a type rule `ptrType::Segment`: arg 0 a string
  terminal, arg 1 a bit-vector numeral, else `errorTy`. A non-numeral `a`
  is a type error; with `--vc-typecheck` (default on) it is reported as
  one. `sea.segment` has its base as an attribute, so OpSem always has
  a numeral. A symbolic `a` is unsupported until a user needs one;
- a constructor, not a marker: it is never stripped like an
  observation and never rewritten into another pointer operator;
- hash-consing gives one node per object: the same `(k, a)` is the same
  node. Two segments with one name and different bases would give two
  nodes; `vc.seam-segment-duplicate` (`lib/VC/OpSem.cpp:808`) stays and
  refuses that, as it does today. Distinct `k` is distinct identity.
  Disjointness of the footprints stays the registry's check
  (`vc.seam-segment-overlap`) and the premises (A-SEP, A-INB), as today.

The name matches `sea.segment`, PtrSort.md's "the segment", and the
`PTR_*` constructor family. The formal model calls it `Term.segment`.

Printing:

- Expr dumps before the boundary (trail text, `--seam-trail-text`,
  `sea-seam-export`, debug logs): `segment(obj.<k>, 0x<a>)`. The `obj.`
  prefix keeps the names that tests and readers grep for; the numeral is
  shown because the formal model reads it off the term.
- `--smt2`: no pointer reaches SMT-LIB. After step 3 of the migration
  (below) the erased term is the numeral `#x...` and the query declares no
  `obj.<k>` symbols. Before step 3 (step 1) the erased term is the
  bit-vector constant `obj.<k>`, declared as today.
- `object:` lines and jsonl `object` records (`tools/sea-vc/sea-vc.cpp:8541-8560`):
  unchanged (`name=obj.<k>`, base, size). The jsonl `symbol` field names
  an SMT-LIB symbol that no longer exists after step 3 (open question 5).

## The invariant

Agreed with the formal-model reviewer: a rewrite before the boundary
preserves the view of every pointer term (same base, or still no view).

A sufficient condition, which the rules must follow:

1. no rule rewrites `ADDR(PTR_SEGMENT(k, a))`, `PTR_TO_INT(...)` or
   `PTR_TO_BITS(...)` of a segment to `a` before the boundary;
2. a base leaves a term only by same-base cancellation
   (`diff.same-base`: `ADDR(seg_k + x) - ADDR(seg_k + y) -> x - y`, and
   `sub.same-base`, `cmp.same-base`, which cancel the same way).

Deciders may read the numeral `a` as a fact to decide a comparison: the
compares of ROW, `ptrEq`, `overlap`, ARM, `cmp.layout`, `mem.in-layout`,
`shadow.distinct-base`, R-STRIDE, the Ackermann decider and InitialWords.
A decision replaces a compare by `true` / `false`. It does not rebuild a
pointer or an offset with the numeral in it.

Agreed per-leaf form (open question 1, settled): per leaf, the protected
set is the view base, or else the mentioned bases; a leaf changes only by
a decided arm or by same-base cancellation; no leaf gains a base. The
unit checks (`sea-seam-rules-test`, segment, random-segments) pin it on
the shape of the fold case and on random reads.

As stated the invariant is about terms. Exact simplifications that drop an
ite arm (a decided condition, a Boolean fact of the fold) turn
`ite(c, seg_1, seg_2)`, which has no view, into `seg_1`, which has one.
Pushing an ite out of an offset followed by `diff.same-base` does the
same to `seg_5 + (ADDR(ite(c, seg_4 + 4, seg_5)) - ADDR(seg_4))` in its
c arm. Both are wanted. A per-leaf statement that admits them and
excludes the fold bug: **every leaf keeps the bases it mentions, except that an
arm is dropped by deciding its condition or a base pair cancels by
same-base cancellation.** The fold bug removed `ADDR(obj.4)` by
substitution, which is neither.

## Where it is built

Only in OpSem, at `sea.segment` under `--seam` (`lib/VC/OpSem.cpp:795-841`):

- `obj = op::ptr::segment(name, base)` replaces
  `op::ptr::ptrConst("obj." + name, w)`;
- `bases.add(obj, ...)` registers the segment node (the registry keys on
  it; `ObjectBase::constant` becomes the segment node, rename to
  `ObjectBase::term`);
- `ctx.addSide(mk<EQ>(mk<ADDR>(obj), base))` is removed: there is no
  layout side condition any more;
- `bases.addAlias(<bv twin>, obj)`: kept in step 1 (the eraser produces
  the twin), removed in step 3 (no twin).

Without `--seam` a segment stays `PTR_FROM_INT(<numeral>)`
(`lib/VC/OpSem.cpp:837`); open question 4.

## Erasure

`SeaOpSem::solverTerm` (`lib/VC/OpSem.cpp:90`, the eraser
`PtrIndexExpander`, `include/sea/Expr/ExprOpPtr.hh:518`, and SeaM's
`Rules::erase`, `include/sea/Expr/SeaMRules.hh:800`) erases
`PTR_SEGMENT(k, a)` to `a`. The store-map and the pointer sort are erased
the same way. `PtrIndexExpander::has` and `expand` learn the operator. After
the boundary nothing can tell two objects apart, so every reader of
identity after the boundary must get it from a record made before it
(next section).

A second, generalizing erasure is needed for the per-firing check.
Today `--seam-check` checks a premise-free firing with `true` as premise,
and the erased bases are free bit-vector constants: the rule is checked
for every base value. Erasing to `a` would check it only at the placed
numerals, which is a weaker check. So `--seam-check` (and `sea-seam-export`)
erase `PTR_SEGMENT(k, a)` to a free constant `obj.<k> : bv(w)`, and add
`obj.<k> = a` only where the firing names LAYOUT or a program premise.
This is what `firingPremise` / `accessPremise` / `layoutFacts`
(`lib/VC/OpSem.cpp:343-450`) state today. The unit drivers (SeaMRules,
StoreMap, MemOps, AddrRange) evaluate the same way (open question 3).

## Consumers of `obj.k` and what each does with `PTR_SEGMENT`

Before the boundary (keys on the registered node; no change beyond the
key, unless noted):

| consumer | where | with PTR_SEGMENT |
|---|---|---|
| `view()` | `SeaMRules.hh:1240-1290` | `isBase(e)` finds the segment node; unchanged |
| `mentionsBase` | `SeaMRules.hh:1190` | the `isBase` test runs before the arity test, so the numeral child is never visited; unchanged |
| `ptrish`, `canonSum`, `sortedSummands` | `SeaMRules.hh:1162`, `:728`, `:591` | unchanged |
| `BvRules::sortSum` (bases first) | `BvRules.hh:129-138` | unchanged; the segment node is made where `obj.k` was, so its creation id, and hence the sum order, stays the same (hypothesis, checked by the step 1 gate) |
| ARM leaves | `AddrRangeMap.hh:585-605` | `isLeaf` must take `PTR_SEGMENT` as a leaf (today: arity-1 FAPP); otherwise it would walk into `a` and see an absolute point |
| registry readers of the numeral (`strideApartEntry`, `outOfObjectSpans`, `layoutDiff`, `shadow.distinct-base`, `rangeCond`) | `SeaMRules.hh:956`, `:1544-1570`, `:5479-5529`, `:3335`, `:2012` | read `ObjectBase::base` as today. `rangeCond` builds `ADDR(value) - ADDR(seg)`; the erasure makes it `value - a` |
| rule walkers that recurse into kids (`rules::isLeaf`, `ExprRewriteRules.hh:25`; the rewriter) | | a `PTR_SEGMENT` is a leaf; rewriting its kids (a string, a numeral) is a no-op either way |
| `Split::IteCensus::ground` | `lib/VC/Split.cpp:1152-1163` | a segment is ground (a fixed address); today it is found by `isFapp` + `isBase`, then by `isOpX<PTR_SEGMENT>` |
| split leaf names (`leaf:obj.heap.1`) | `test/VC/split/alias.mlir` | print `obj.<k>` (the name, not the dump) so the names stay |
| `firingPremise`, `accessPremise`, `layoutFacts` | `OpSem.cpp:343-450` | the LAYOUT conjuncts `obj.<k> = a` over the generalized erasure (above); without it they are tautologies |
| `sea-seam-export` (`side:` lists the layout equations) | `tools/sea-seam-export/sea-seam-export.cpp:208`, `:239` | no layout side conditions; terms print `segment(obj.<k>, 0x<a>)`, which the formal model reads |

After the boundary (the erased query; today they key on the erased twin
`obj.k : bv(w)`, `BaseRegistry::addAlias`):

| consumer | where | step 1 (twin) | step 3 (numeral) |
|---|---|---|---|
| Ackermann layout values | `sea-vc.cpp:6847-6859`, `Ackermann.hh:942` | unchanged | dropped: the numeral is in the term. The decider's first pass ("without the layout values", Ackermann.md) now sees them; `vc.ackermann-decider-pairs-layout` counts 0 (a counter change, stated) |
| InitialWords alignment proofs and atoms | `InitialWords.hh:113-117`, `:332`, `:429` | unchanged | the layout map dropped; a symbolic part S no longer holds the base, so `obj.k + x` and `obj.j + x` share one atom (words mode only; equisatisfiable, gated) |
| ARM table `keyOf` (narrowed reads) | `sea-vc.cpp:7189`, `Ackermann.hh:1127` | unchanged | key by object order carried in `NarrowTable::Entry` (`Ackermann.hh:387`), not by the erased base node: a numeral is not an identity |
| narrow / own tables (`nbase`, same-object test `rs[k].nbase == rs[l].nbase`) | `sea-vc.cpp:7138-7150`, `Ackermann.hh:795` | unchanged | the entry carries the object order; `nbase` is only the term to build |
| stride `size0` bases | `sea-vc.cpp:7211-7216`, `:7284-7290`, `Ackermann.hh:1229` | keyed on the pre-boundary node (`op::ptr::base` of recorded nodes): the segment node | same |
| cells plan object info | `sea-vc.cpp:7296-7302` | unchanged | the erased base becomes `a`; the object is already identified by `o.order` |
| eq audit shapes (`O`, `N`) | `tools/sea-vc/EqAudit.h:234-237` | unchanged | a base reads as `K`; the audit's shape classes for objects need a pre-erasure record or are lost (open question 6) |
| eq audit and `--narrow-offsets=check` obligations (`layoutFacts`) | `sea-vc.cpp:8027`, `:8301` | unchanged | `true`; the numerals are in the obligations |
| model trail (FoldTopEqs step 2) | `FoldTopEqs.md`, "Model trail" | unchanged | the `def obj.<k>` entries disappear (`vc.model-trail: defs=` drops by the number of objects) |
| `--vc-defs` (SumNames) | `include/sea/Expr/SumNames.hh` | unchanged | the bases are numerals, as they are today after the fold's step 2 (FoldTopEqs.md Open); with the fold off, sums group differently (`test/VC/vc-defs/cursor-sat.mlir` NEST changes) |
| Origins / `--arith-stats` | `include/sea/VC/Origins.h` | unchanged | `along()` stops at the segment (another operator); numerals carry no origin today, so no arithmetic node loses one (hypothesis) |
| model evaluation, replay | `lib/VC/Replay.cpp:321`, "Models" in PtrSort.md | a pointer constant through its twin, as today | the segment register evaluates to `a`; `test/VC/segment-replay.mlir` pins "the segment's value is its base", which now holds by construction: recreate it to check the replayed run's segment values |
| model lookup by `obj.<k>` name | the trail, `--model-trail-out` | unchanged | no such symbol |
| place-out / place-in files | `lib/Transforms/PlaceStatic.cpp:44-57` | MLIR level, keyed by segment name: unchanged | unchanged |
| `query-diff` SUBST of `obj.<name>` | `utils/query-diff.py:300-330` | unchanged | needed only to compare an old binary against a new one (the old side defines `obj.<k>`, the new has numerals); a new-vs-new comparison has none |
| `encoding-diff` rename `obj.<segment>` -> `obj.<alloca label>`, placement conjuncts | `utils/encoding-diff.py:20`, `:278-295`, `:393` | unchanged | no `obj.` symbols: the object numerals of two placements differ unless the placement is shared (`--place-out` / `--place-in`); the tool must compare on one placement or map numerals (open question 7) |
| `cover-diff`, `memops-equiv`, `jsonl-check` | `utils/cover-diff.py:50`, `:95`, `utils/memops-equiv.py:24`, `utils/jsonl-check.py:113` | unchanged | follow query-diff / the `symbol` field decision |
| help and `--agent` texts naming `obj.<name>` with a side equality | `sea-vc.cpp:577`, `:2404`, `:2447`, `:3137`, `:3296`, `:3892`, `:4350`; `SeaOps.td:1041`, `:1094`; `include/sea/Expr/README.md:62` | step 1 changes the wording (no side condition before the boundary) | the query has numerals |

Tests that match `obj.` (24 files; `grep -rl 'obj\.' test`): the
`--smt2` checks `test/VC/segment-encoding.mlir` (SEAM: `(= obj.stack.3
#x...)`), `test/VC/seam/eraser.mlir`, `test/VC/narrow/two-objects.mlir`,
`test/VC/vc-defs/cursor-sat.mlir` and `test/Tools/encoding-diff.test`
change in step 3. The `sea-seam-export` checks in `test/SeamExport/*`
change with the dump syntax. The split, jsonl and `object:` line checks
keep their names. All are disposable; each change is named in its
commit.

## SeaM rules that change

Rules that read or produce `ADDR(obj.k)` or the layout facts:

- `rangeCond` (`SeaMRules.hh:2004-2016`): produces `ADDR(value) -
  ADDR(o.constant)`; the object term becomes the segment, otherwise
  unchanged. It must not fold `ADDR(seg)` to `a` (invariant, condition 1).
- `cmp.layout`, `mem.in-layout` (`layoutDiff`, `SeaMRules.hh:5479-5529`;
  only with `layoutDecide` and without the premises): read
  `ObjectBase::base`. The option comment ("valid only where the layout
  equations obj = n are part of the formula", `SeaMRules.hh:171-175`)
  becomes: valid for every segment term. The unit drivers' "free constant
  bases" become segments evaluated by the generalizing erasure.
- `shadow.distinct-base` (`SeaMRules.hh:3335-3337`, `:5081-5088`, `:5160`):
  reads the numerals; PremLayout means "the segments' numerals" instead
  of "the side equations". Still a fact, not an assumption, never on the
  `assume.*` ledger.
- R-STRIDE (`strideApartEntry`, `SeaMRules.hh:953-990`): reads the layout
  numeral; unchanged.
- `diff.erase` (PtrSort.md): `PTR_DIFF(p, q) -> ADDR(p) - ADDR(q)` across
  bases keeps the `ADDR(seg)` terms; the erasure turns them into numerals.
  `diff.same-base`, `diff.ite-same-base`, `sub.same-base`: the only
  places a base may leave a term.
- `ptr.to-int-view` (`SeaMRules.hh:1259-1263`): reads `ADDR(seg)` as
  `seg`; unchanged.
- The `Premise` comments that define LAYOUT as "the placement equations
  obj.k = <numeral> (side conditions on every path)"
  (`SeaMRules.hh:66-70`, `:307-317`) are reworded.

No rule today turns `ADDR(obj.k)` into a numeral, because `obj.k` has no
numeral to read. With `PTR_SEGMENT` a rule could, so this becomes a check:
a unit test runs every rule on `ADDR(seg)`, `PTR_TO_INT(seg)`,
`PTR_TO_BITS(seg)`, `PTR_DIFF(seg_a, seg_b)` and `seg + x` sums, and
checks that no result has a segment's numeral where an `ADDR(seg)` was.

## What F2 (`--vc-fold-top-eqs`) does then

Step 1 (the fold, before the boundary). There are no layout facts: no
conjunct `ADDR(obj.k) = c` exists before the boundary. The population
the interim rule skipped on the step jobs (FoldTopEqs.md: "mostly the
placement's layout facts") is gone.

Can `addressDerived` be dropped? No. A program fact on an address,
`assume((uintptr_t)p == K)` with `p = seg + x`, gives `PTR_TO_INT(seg +
x) = K`. Substituting it removes the base `seg` from every term that
contains it, and not by cancellation, which breaks the invariant.

Can it be narrowed? Proposed: skip T iff a `PTR_SEGMENT` is reachable from
T without passing a read (select, wselect, store, wstore). That is
`Rules::mentionsBase` (`SeaMRules.hh:1190`), the test views already use.
The argument (a hypothesis until the gates below hold): replacing a
subterm that mentions no base by a numeral removes no base mention from
any term, so every leaf keeps its bases (the per-leaf invariant). A fact
on a read replaces the read whole. `mentionsBase` does not look into
reads, so an offset that contains the read mentions no base before or
after. The narrowed rule admits facts the interim rule skips: an address
of a pointer register (`ADDR(p) = c`, p a block argument or a nondet
pointer, not a base), `PTR_TO_INT` of such a pointer, and a `PTR_DIFF`
of two such pointers. The counter `skipped-ptr` becomes `skipped-base`.

Step 2 (the elimination, after the Ackermann lowering). Step 1 of the
migration: unchanged, the erased query still has `obj.<k> = a` and step 2
eliminates the bases. Step 3: no object bases exist in the erased query;
the trail has no `def obj.<k>` entries. Step 2 still eliminates the
Ackermann reads, nondets and the exit guard.

## The erased query

Step 1 (erase to the twin `obj.<k>` and add the layout conjuncts at the
boundary): the erased query is expected to be byte-identical to today's
for every job. Conjunct order can move only if the boundary adds the
layout conjuncts at a different place than the side conditions had (then
`order`). Gate: `utils/query-diff.py` (it turns the fold off), the four
`--seam` configurations; expected `identical` (or `order`), no other
class. A hypothesis that needs this gate: no rule fired on the layout
side conditions in a way that changed another conjunct.

Step 3 (erase to `a`): the query changes. Each base is a numeral, and
the flat sums merge it with the constant offset (`bvadd obj.k x #x4`
becomes `bvadd x #x1004`). The layout conjuncts and the `obj.<k>`
declarations are gone. It is equivalent to the step 1 query with
`obj.<k>` substituted by `a` (query-diff's SUBST does exactly that for
the one-sided symbols). Gate: `query-diff --project-blocks` against the
step 1 binary; expected `identical` / `order` / `equiv-sat` /
`both-unsat` with `subst=<objects>`, no `DIFF`. With the fold on (the
default) the solver-facing query today already has numerals after
step 2, so the default query changes less than the fold-off query.
`encoding-diff` is not the gate here (one program, two binaries).

## Migration

Cheap per-step gates; one attestation at the top of the stack.

1. **The term, erased to the twin.** `PTR_SEGMENT` in ExprOpPtr.hh
   (type rule, printer), OpSem builds it, the registry keys on it, the
   eraser maps it to the bit-vector `obj.<k>` and the boundary adds
   `obj.<k> = a` to the erased query. The ARM leaf and the split and
   ground walkers learn the operator. `--seam-check` keeps today's
   premise construction. Gates: `check-sea`; query-diff (above),
   expected identical; `fuzz-seam --seam-check` (wide, cells, bases;
   seeds 1, 2, 3, 5, 7; three programs each) with no invalid firing and
   `unexpressible` not above the base; the step jobs' Ackermann pairs
   436 / 436 / 161 / 146 (`--lower --seam --widen-memory=4 --vcgen-only`).
2. **Narrow the fold's rule** to `mentionsBase` outside reads, with a
   hidden testing switch that turns it off. Gates: the
   `sea-fold-top-eqs-test` ptr-facts cases (a base-free address fact is
   now substituted, an address with a base is still skipped);
   `test/VC/fold-top-eqs/layout-facts.mlir` with the switch off
   (`unexpressible=0`); fuzz-seam `--seam-check` as in step 1; step pairs
   unchanged.
3. **Erase to `a`.** The eraser maps the segment to its numeral; the
   boundary adds no layout conjuncts; `addAlias` and the twin go;
   `--seam-check` and `sea-seam-export` use the generalizing erasure; the
   post-boundary consumers move to object order (ARM / narrow / own
   tables), the layout values go (Ackermann, InitialWords). Gates:
   `check-sea`; query-diff `--project-blocks` against step 1 (above); the
   step pairs (expected unchanged: the decider already had the numerals
   through the layout values. Hypothesis: reads of two objects at one
   numeral address now hash-cons to one read, which needs an
   out-of-object address and is not expected on these jobs; any change is
   explained before the step lands); `--seam-words` jobs through
   query-diff (the atom change of InitialWords); fuzz-seam `--seam-check`;
   `fuzz-vc` smoke.
4. **Docs**: PtrSort.md (Constructors, The boundary), SeaM.md
   ("The VC keeps bases symbolic", Soundness stance, Validation),
   FoldTopEqs.md (Pointer observations, step 2, Open), Ackermann.md
   (layout values), `include/sea/Expr/README.md`, the help and `--agent`
   texts.

Benchmarks before the top of the stack is attested: firedancer and vcc,
the default configuration of the base against the head
(`utils/bench.py`), verdicts identical, Ackermann constraints and
`vc-time` compared. A solve-time change on a job with an identical
query is variance. On a changed query it is examined (z3 verbosity
first, seeds for variance) before the step lands.

## Acceptance

- `test/VC/fold-top-eqs/layout-facts.mlir` passes without the interim skip
  (the switch of step 2 off): `result: sat`, `seam-check: ...
  unexpressible=0`.
- The step jobs' Ackermann constraints stay 436 / 436 / 161 / 146.
- `fuzz-seam --seam-check`: `unexpressible` no worse than the base, no
  invalid firing.
- Step 1's query-diff: identical (or order) on every job; step 3's: no
  `DIFF`, no `unknown` left unexplained.
- `--seam-check` checks premise-free firings at free bases, as today (the
  generalizing erasure).
- No new `vc.typecheck` error on the corpora; no
  `vc.ptr-sort-at-boundary`.

## Open questions for the maintainer

1. **The invariant's wording.** Settled: the per-leaf form ("The
   invariant": per leaf, the protected set is the view base, or else the
   mentioned bases; a leaf changes only by a decided arm or by same-base
   cancellation; no leaf gains a base). "Same base, or still no view" per
   term excluded exact ite simplifications that the fold and SeaM rely on
   (a decided condition turns a no-view ite into a segment).
2. **Stop after step 1?** Step 1 removes the fold hazard (no layout facts
   before the boundary) with a byte-identical query and no change after
   the boundary. Step 3 is what the formal model wants (the layout read
   off the segment, no layout obligation on the query). It costs the
   identity of every post-boundary reader (the table above) and changes
   the query. The note assumes both steps, as decided; step 3 can wait
   for a need.
3. **Unit drivers.** They use free constant bases today so that every
   rule is tested at every base value. Option (a): they build segments
   and evaluate through the generalizing erasure. Option (b): the
   registry also accepts plain pointer constants as bases in the
   drivers only. (a) tests what sea-vc runs; (b) is less churn.
4. **Without `--seam`.** A segment stays `PTR_FROM_INT(<numeral>)`. Using
   `PTR_SEGMENT` there too would make `PTR_FROM_INT` mean int-to-ptr only
   (`vc.ptr-from-int-opaque` would no longer count segments, if it does
   today), and after step 3 the erased query is identical. Do it in this
   series or later?
5. **The `symbol` field** of jsonl `object` records (`obj.<k>`, checked
   by `utils/jsonl-check.py:113`) names an SMT-LIB symbol that step 3
   removes. Keep it as the dump name, or drop it?
6. **Eq audit shapes** (`O` / `N` for object bases) are lost after step 3
   unless the audit gets a pre-erasure record. Is the audit's object
   class still needed?
7. **`encoding-diff`** renames `obj.<segment>` to the alloca label so two
   lowerings' objects match. After step 3 the objects are numerals that
   differ between placements. Require a shared placement
   (`--place-out` / `--place-in`), or map numerals to objects from the
   `object:` lines?
8. **`--smt2` readability.** Option: write one comment line per object
   (`; segment obj.<k> #x...`) so a reader of the query can map numerals
   back. The comparison tools skip comments.
9. **The `--vc-defs` interaction** (FoldTopEqs.md Open): with numeral
   bases, prefix sums of different objects share their symbolic part and
   group differently. This is the same as today with the fold on, but
   new with the fold off. Measure `--vc-defs=hidden` on the jobs that use
   it.
