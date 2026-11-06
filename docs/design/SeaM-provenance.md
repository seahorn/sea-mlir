# SeaM: keeping provenance when reading through memory operations

Status: Step 0, pruning impossible size-zero address leaves, is
implemented. The optional v1 split rule is described below. The order is
step 0, measurement, then v1 only for cases that remain.

## Why

A guarded read-table prototype (`--x-read-table`) took array_list_swap
from 81.6 s / T/O / 51.6 s (Yices / z3 / Bitwuzla, Ackermann on) to
1.3-2.5 / 6-9 / 2.5-3.4 s. Its gain came from two things,
not from the symbol allocation policy:

1. provenance: the read address split into (object, offset) leaves where a
   read is resolved through a copy, with the base cancelled inside one
   object, so the translated source read keeps its object and a base-free
   offset;
2. removing branches that A-INB makes impossible (a read leaf in the null
   object; a hit between two different objects).

Production SeaM removes impossible branches by conditional equivalence
(`premises => lhs = rhs`), as every other SeaM rule; the prototype's
fresh-value relaxation stays an optional, named abstraction. The
semantics-only boundary stays the definition of the terms; provenance
reasoning lives in the rewriter, with a helper shared with the boundary if
needed.

## Where provenance is lost today

`Rules::rowMemOp` (SeaMRules.hh) decides `select(T, idx)` over a memory
term T with `decideIn(idx, d, n)`. `decideIn` already compares the leaves of
idx and d pair by pair (`eqPairs`, `decideInPair`), but it returns one
answer for the whole read: In, Out, or Unknown. A mixed read (some leaf
pairs Out, others Unknown or In) is Unknown, and rowMemOp builds

    ite((idx - d) <u n, hit(idx - d), select(M, idx))

over the whole terms. `idx - d` with `idx = ite(c, null + o1, obj.9 + o2)`
and `d = obj.3` keeps every base; the translated source read
`s + (idx - d)` then mentions all objects, and no later step recovers which
object it addresses.

## Step 0: prune impossible leaves of access addresses (rule addr.prune-size0)

On a read, prune the index term from null objects based on A-INB.

For an access (load, store, memset / memmove / memhavoc destination or
source) whose address is `a = ite(c, null + o, B + x)`: A-INB says the
access is inside an object; the null object (size 0) holds no
positive-width access, so `c` is false on every execution that satisfies
this access's A-INB, and `a = B + x` there. Hence, as a conditional
equivalence,

    select(M, ite(c, null + o, B + x))  ~>  select(M, B + x)
    store(M, ite(c, null + o, B + x), v)  ~>  store(M, B + x, v)

with premise the access's own A-INB, `inBounds(a)` = `!c && inBounds(B + x)`
(the guarded aggregation of SeaM-null.md). Generally, for a positive-width
access (load, store): drop every syntactic ite leaf of the address that is
in a size-0 object, keeping the guards of the others; if no leaf is left,
the access is a definite violation (existing static fallback).
Scope of the first implementation: syntactic ite leaves recognized
as size-0 only. Removing an entry of an ARM map is not pruning: ARM entries
over-approximate and give no replacement expression; broader ARM pruning
needs its own reconstruction and premise contract (later).

- Ranges (mset / mcopy / mhavoc destination, mcopy source) are different.
  Pruning a range is an operation-level equivalence,
  not address equality and not unreachability of the removed branch. With
  `d = ite(c, null, B + x)` and n = 0, A-INB permits c = true; replacing d
  by B + x is valid because both operations are no-ops when n = 0, although
  the addresses differ. So:
  - mixed range (a live leaf left): `op(.., ite(c, null, B + x), ..)` ~>
    `op(.., B + x, ..)`, checked as equality of the two memory values under
    the original range premise (`n == 0 || inBounds of the range`), not as
    `d = B + x`;
  - all leaves size-0: A-INB implies n = 0, so the result is the incoming
    memory (the operation is a no-op), under the range premise; this is
    NOT a violation. Only when n > 0 is established does the existing
    violation handling apply; with symbolic n there is no static violation;
  - the same for an mcopy source.
  This covers array_list_swap's copies 2 and 3, written through phi pointers
  that join null and a live pointer (%v116, %v121), without splitting the
  destination.
- Access positions only: the rewrite replaces the index / address argument
  of the access node. The shared address term itself is unchanged; its
  other uses (a pointer compare, a pointer stored as a value, ptr.base) keep
  the null alternative. Never prune a value or a program compare.
- Numeral (absolute) leaves are kept: a numeral may be a valid address
  (array_list_swap has a numeral alternative on the getter-failure path).
- Shadow memories are exempt (identity contract, no A-INB).
- Assurance, loads and stores: the firing's lhs is the access, so
  `SeaOpSem::firingPremise` builds the premise from its own address
  (satisfiable while one live leaf remains); `--seam-check` checks
  `inBounds(a) => lhs = rhs` through the boundary. No context-dependent
  cache key: the premise depends only on the access node.
- Assurance, ranges: step 0 extends the checker. Today
  `Rules::accessAddresses` takes only SELECT / STORE, SeaMRules.hh:691;
  `accessRanges` only the destination range of a memop directly below a
  SELECT, :710; `firingPremise` needs a scalar access width,
  OpSem.cpp:157-215. A range-pruning firing records exactly which original
  range(s) it used (destination, source), captured before the rewrite, so
  pruning cannot hide the condition that justified it; the obligation is
  equality of the two memory values (`firingObligation` already compares
  memory-valued sides at a fresh index) under that original range premise,
  never a premise rebuilt from the pruned address. The bounds job remains
  the discharge mechanism.
- After step 0 many reads have a single-leaf address (obj.9 + o), and
  today's whole-read decision (cmp.diff-base against the temporary) decides
  them; the split of v1 is then needed only for what remains.
- Tests: a fixed-destination null / live read (premise satisfiable, null
  leaf pruned); a store through null-or-live; a copy whose destination is
  null-or-live with symbolic n (n = 0 admitted); a numeral leaf kept; a
  shadow access not pruned; a program compare of the same pointer not
  pruned; an all-null positive-width read -> fallback; and the range cases:
  an all-null source / destination with n = 0 stays a no-op; a
  mixed null / live range with n = 0 and the null guard true validates
  operation equivalence without address equality; an all-null range with
  symbolic n under n = 0 (no static violation). Measure array_list_swap and
  hash_iter_begin (default row expansion, three solvers) and report which
  undecided shapes remain.

## Step 0: implementation and measurement

- Rules (SeaMRules.hh): `Rules::pruneSize0` (the syntactic ite leaves of an
  address whose pointer view is in a size-0 object; numerals and leaves
  without a view kept). It enters only on a top-level ite: a bare address
  into a size-0 object (one leaf) is not rewritten by this rule (a positive-
  width access there is the static violation; a range there is left to the
  existing rules, e.g. cmp.size0-vacuous for a read over it), so the rule
  does not normalize every such range to a no-op; `pruneAccess` at select / store (counter
  `addr.prune-size0`, premise A-INB, the index argument only; no rewrite
  when every leaf is size 0, or for a shadow memory); `memOp` at an mset /
  mcopy / mhavoc term (`addr.prune-size0-range`: a range with a live leaf
  left; `addr.prune-size0-noop`: every leaf of a range size 0, the term is
  its incoming memory; no rewrite when the range condition is `false`, a
  numeral n > 0 that cannot fit: the static fallback). Off without A-INB and
  with `sea-vc --seam-prune-size0=0`. SeaOpSem rewrites each term after
  recording its ranges' A-INB from the unpruned addresses.
- Checker: `Rules::accessRanges` also returns the ranges of a firing whose
  lhs is a term (destination; and source for mcopy), taken from the lhs,
  i.e. before the rewrite; `firingPremise` uses them for such a firing
  whatever access context it ran under; the obligation is the existing
  memory equality at a fresh cell (`firingObligation`). A planted wrong
  range prune (keeping the null leaf instead of the live one) is reported
  `invalid=1` (manual check, not a test).
- Tests: unit tests `prune-size0` in sea-seam-rules-test (load, store with
  the pointer as value kept, nested, numeral kept, zero-size allocation,
  all-null not pruned, compare not pruned, no A-INB / option off, shadow) and
  in sea-memops-test (mixed destination with symbolic n, including n = 0
  with the null guard true and different addresses; all-null destination
  with symbolic n: no-op, n = 0, no static violation; all-null with numeral
  0 and with numeral 2; mixed and all-null mcopy source; a read after
  pruning decided by cmp.diff-base; no A-INB / option off); each checked
  valid under the range premise and invalid without it. VC test
  `test/VC/seam/prune-size0-ranges.mlir` (dst, src, all-null, --seam-check
  with z3 and Yices). The null tests that pinned cmp.size0-vacuous pin
  addr.prune-size0 and keep the old path under --seam-prune-size0=0.
- Measurement, array_list_swap (vcc corpus, `--lower --seam`, default row
  expansion; wall s; Yices / z3 / Bitwuzla):
  - before (prune off): 75 / T/O (300) / 10.5; 108 reads over a copy
    undecided (`row.mcopy`);
  - step 0 alone: 74 / 192 / 6.2; 98 undecided. Of these, 82 had an index
    mentioning two objects: the translated read address
    `S3 + (ite(c, S9 + a, S9 + a + 128) - (S9 + b))` (the list getter's two
    alternatives, both in the list's storage S9, minus the copy source in
    S9): the base cancels in each leaf, but no rule cancelled it through the
    ite (`sub.same-base` needs a pointer view on both sides);
  - step 0 + `sub.ite-same-base`: 1.5 / 3.3 / 3.4; 22 undecided, every one
    a read in the copy's own object at symbolic offsets (element i vs
    element j: not decidable by rules). The x-read-table prototype, for
    comparison: 1.3-2.5 / 6-9 / 2.5-3.4.
  - `sub.ite-same-base` without step 0 does nothing here (108 undecided):
    the null leaf is what made the ite leaves mixed-base.
  - hash_iter_begin: 1.25 / 2.7 / 7.9 -> 1.3 / 2.3 / 6.3 (490 prunes).
  - `--seam-check` on array_list_swap (z3, 10 s per check): 918 firings,
    891 valid, 0 invalid, 27 unknown (timeouts, including existing rules
    such as add.pull-ite: the boundary's translation of deep copy chains).
    Evidence, not complete validation: the 27 are not passed (rules:
    row.mcopy-skip + cmp.diff-base 11, add.pull-ite 7, addr.prune-size0-range
    3, addr.prune-size0 2, sub.ite-same-base 2, cmp.same-base + cmp.ite-push 1,
    row.mcopy-skip with arm.skip 1). With 60 s per check: 901 valid, 0
    invalid, 17 unknown (row.mcopy-skip + cmp.diff-base 10, addr.prune-size0
    2, addr.prune-size0-range 2, sub.ite-same-base 1, cmp.same-base +
    cmp.ite-push 1, row.mcopy-skip with arm.skip 1): not passed.
  - vcc (228 jobs, `--widen-memory=8`; bench run
    same binary, prune on vs
    `--seam-prune-size0=0`, sub.ite-same-base on in both): no sat / unsat
    disagreement; Yices 542 -> 475 s total, z3 748 -> 454 s, z3 solves
    array_list_swap (T/O -> 4.0 s; Yices 91 -> 1.7 s); jobs solved by both:
    Yices 240 -> 172 s, z3 295 -> 299 s. hash_table_eq is a timeout /
    unknown in both (not step 0: the Ackermann lowering, below the plan's
    scope).
- `sub.ite-same-base` (new, exact, no premise): `ite(c, A + x, A + y) -
  (A + z)` (either side the ite; every leaf, at most maxPullLeaves, a
  pointer into the other side's object) ~> the ite of the differences, each
  then cancelled by sub.same-base. This rule is separate from the earlier design.
- `sub.ite-same-base` is exact independently
  of A-INB, placement, disjointness or no-wrap; the all-leaves-same-base
  requirement is a profitability restriction, not a premise; the two-ite
  case stays outside the rule.
- Consequence for v1 (deferred work): on array_list_swap no read is left
  whose leaves are in different objects, so the split is not needed there;
  the null-phi destination example of the v1 sections is substantially
  addressed by step 0. Decide on v1 from corpus measurements (provenance
  counts and undecided-read shapes alongside time, within one controlled
  ablation).

## v1: split the read over its address leaves (rule row.memop-split)

When `decideIn`'s summary is mixed (some leaf pairs decided) or the split
exposes a same-base cancellation (see Trigger below), and idx is a
top-level ite tree within the index-expansion budget (separate from the
comparison budget `maxDecideLeaves`, see Budget), distribute the read over
the leaves of idx:

    select(T, ite(c, i1, i2))  ~>  ite(c, select(T, i1), select(T, i2))

(an exact identity: select over ite of the index). Each leaf read is then
decided on its own by the existing rowMemOp: most become Out (null leaf vs
the destination: cmp.size0-vacuous; another object: cmp.diff-base, both
A-INB with the whole-range premise) or In (same object, decided range), and
an undecided leaf gets its own `ite(k <u n, hit(k), miss)` with
`k = leaf - d` in which a common base cancels (`(B + o1) - (B + o2) =
o1 - o2`, mod 2^w, premise-free). The translated source read of a hit is
`s + k` with s's object and a base-free k: the provenance.

- Guards: the ite conditions of idx stay in the term; each leaf read is
  relevant only under its guard. A leaf decided Out under A-INB is removed
  by conditional equivalence: the premise is the A-INB of that leaf's access
  (guarded), not of the other leaves.
- d with several leaves: v1 splits only idx; d stays whole unless it is one
  leaf. Known miss: array_list_swap's copies 2 and 3 write through
  phi pointers (%v149 through %v116, %v155 through %v121; %v121 joins null
  and %v105), so v1 does not cover them and must not claim the prototype's
  results before measuring. Extension (v2): split d exactly,
  `select(copy(M, Ms, ite(c, d1, d2), s, n), j) -> ite(c, select(copy(M,
  Ms, d1, s, n), j), select(copy(M, Ms, d2, s, n), j))` (same for set /
  havoc; the source snapshot Ms unchanged), or build the guarded per-pair
  read equations directly; cofactor idx, d and s together when they share a
  guard (two compatible pairs, not four; eqPairs already does this for
  decisions).
- Scope of v1: top-level ites of idx only. `ite(c, A, B) + o` is not a
  top-level ite and is a known miss unless normalization exposes it; no
  global ite distribution over arithmetic (expose guarded leaves locally in
  the read decision, exact fallback). Record what v1 misses (counters).
- Trigger: not only "at least one leaf decided". Split also when it exposes
  a same-base cancellation (idx = ite(c, B + x, B + y), d = B + z: both
  decisions Unknown, but both hit offsets lose B and the source keeps its
  object). `decideIn` returns a summary that distinguishes mixed, all
  unknown, and cap exhausted (one traversal), and the policy uses it.
- Same-base subtraction: left to the existing `Rules::sub` normalization.
- Budget: the comparison budget (`maxDecideLeaves`, which counts leaf
  *pairs* in `eqPairs`) is separate from a new expansion budget (index
  leaves per split, and generated leaf reads per VC, across copy chains).
  Keep the whole-read In / Out decision and the ARM fallback first (no split
  of a read already decided). Reject an expansion before building the full
  product. Counters: cap hits, generated reads, reads with lost provenance.
  On exhaustion keep the exact unsplit form; a declined split may be cached
  (correct, possibly less precise; documented).
- Caching: exact splits and fixed conditional schemas are context-free and
  share the rewriter cache; any simplification that uses an enclosing guard
  or hit/miss fact must key the cache on that context or stay outside the
  context-free cache.
- Sharing: leaf reads are hash-consed terms; the rewriter's cache keys them
  as usual. The same leaf read from different parents is one node.
- Mutually exclusive hit/miss tests of the same copy range in one context
  (the mutually-exclusive hit/miss rule) are a separate rule after this one.

## Assurance

- row.memop-split is an exact identity (no premise). The per-leaf
  decisions are today's rules, but checking them "as usual" is not enough:
  `SeaOpSem::firingPremise` derives bounds from the isolated firing
  lhs (lib/VC/OpSem.cpp:157-215), so a null-leaf child firing has an
  impossible local premise and is unexpressible. v1 checks the composed
  parent rewrite: the original read against the reconstructed result,
  through the exact boundary, under the original read's guarded access
  premise `AND_i (g_i => P_i)` plus layout / range premises (P_i = false
  for a null leaf: the premise excludes that leaf only). Scope of the
  composed parent rewrite: the split plus one
  read-over-current-memop decision per leaf; deeper reads (through older
  copies) are later firings, checked separately. A parent covering fully
  normalized descendants would need the guarded premises of every
  conditional descendant rewrite, which this design does not do. This also tests the
  transfer of premises to the leaves. A translated source read carries the
  copy-hit guard (bridge: in-range read + source-range bounds implies bounds
  of `s + (j - d)`); bounds are never asserted unconditionally for both
  branches. Local A-INB rewrite checks only; the bounds job keeps its role.
- Unit tests: the identity; per-leaf decisions under premises, invalid
  without them; budget exhaustion keeps the exact form.
- Equivalence: memops-equiv.py and the memops corpus; fuzz-seam modes with
  pointers that are ites over objects (null alternatives) read through
  copies.
- Targeted acceptance cases; destination-ite and ite-under-
  arithmetic are correctness / fallback coverage outside v1, not required
  to gain provenance; start with a fixed-destination null / live split
  (satisfiable parent premise, unreachable null child): fixed destination; idx and d with a
  shared guard; independent guards; all-Unknown same-base offsets; an ite
  beneath an addition (expected miss in v1); inactive null read vs
  zero-length null range; copy-source bounds under the hit guard;
  overlapping copies; cap fallback.
- Measurement: the original corpus with ordinary row expansion, reporting
  which shapes stay unsupported; array_list_swap and hash_iter_begin with
  all three solvers; the x-read-table numbers are a comparison point, not a
  threshold. Then vcc-small and the vcc bench (no unexplained verdict
  change; conditional unsats from null-only failures recorded).

## Open

1. Plain stores: not in v1. `row()` over a store already splits the
   compare (rowCompare / buildEq) and does not translate the hit address to
   another object; measure later whether splitting a store read's miss
   continuation keeps useful provenance through older copies.
2. Leaf budget values; interaction with ARM (which looks through ites when
   the leaf count is exceeded).
3. d as an ite over objects: v2 (see above).
