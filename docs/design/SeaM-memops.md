# SeaM: memory operations as terms

Status: Step 1 (MCOPY / MSET / MHAVOC, read rules, boundary, and `--seam`
without `sea-expand-memset`) is implemented. Steps 2 (MFILL) and 3 (word
layouts) are not started.

## Why

`aws_array_list_swap` (verify-c-common) is 17 s with Yices without SeaM
and times out with it (z3 and Bitwuzla slow down too). The investigation
found that the VC expands every memory operation into cells:

- constant length: one load / store per cell (`lib/VC/OpSem.cpp:495-525`):
  three 128-byte memmoves become three 128-store chains;
- symbolic length: `sea-expand-memset` first rewrites the program into a
  bounded CFG of K blocks with `assume(n <= K)` (an explicit
  under-approximation, warning `memset.symbolic-bound`), one store each.
  sea-vc also reports the bound as `assume.memset-bound: count=<n>
  bound=<K>` before `result:` (an unsat holds only for lengths <= K). The
  expansion runs only in `--lower` without `--seam`
  (`lib/Transforms/Passes.cpp:221-225`). A symbolic-length memset / memmove
  on a widened word memory is an error (`memset.word-layout`), not a bound.

SeaM then decides read-over-write per *cell*: a read of the list compared
with the 128 cells of the temporary, each compare an ite when undecided.
SeaHorn's lambda encoding instead keeps a copy as one range test
(`BvOpSem2MemRepr.cc:560-684`):

    read(copy(Md, Ms, d, s, n), j) = ite(j in [d, d+n), read(Ms, s + (j - d)), read(Md, j))

and Yices solves SeaHorn's query in 12.8 s. SeaM aims to combine lambda-like range terms with simplification, so it
must keep the operation rather than expand it into cells.

## The terms

Memory-valued Expr operators, like `STORE_MAP` (`StoreMap.hh`): the VC's
memory values may hold them; no solver sees them (the boundary,
`SeaOpSem::solverTerm`, removes them).

| term | from | meaning (cell j of the result) |
|---|---|---|
| `MCOPY(Md, Ms, d, s, n)` | `sea.memmove` | in(j, d, n) ? Ms[s + (j - d)] : Md[j] |
| `MSET(M, d, n, v)` | `sea.memset` | in(j, d, n) ? v (one unit; a word cell is k copies) : M[j] |
| `MHAVOC(M, H, d, n)` | `sea.memhavoc` | in(j, d, n) ? H[j] : M[j] (H a fresh array, as now) |
| `MFILL(M, d, bytes)` | `sea.memfill` (new op) | in(j, d, len) ? bytes[j - d] : M[j] |

- `in(j, d, n) := (j - d) <u n`, modulo 2^w. One compare; no no-wrap
  premise; n = 0 gives false (a no-op). Exact for wrapping ranges too: with
  k = (j - d) mod 2^w and n < 2^w, the cells d, d+1, ..., d+n-1 mod 2^w are
  distinct and j is one of them iff k < n; the copied byte is
  Ms[s + k]. Small-width checks against a guarded-store expansion confirm the
  circular semantics; keep it as the base.
- `Ms` is the memory version the op reads (its source operand), so overlap
  is exact: a memmove reads the source as it was before the op.
- Units: the terms are for byte (index-unit) memories first. Word
  memories keep today's path: `lib/VC/OpSem.cpp:462-516` already encodes
  whole-cell constant lengths (divisibility check, length / units, memset
  byte repeated into a cell) and refuses partial words; that stays as the
  fallback. Word-range terms (n in cells, d and s 0 mod k) are step 3; the
  byte-to-cell conversion stays at one named place.
- `MFILL` is new: SeaHorn's `MemFill` (global initializers: a byte buffer
  stored at the global's address, `BvOpSem2Context.hh:293`) next to
  `MemHavoc`. Today the exporter stores initializers byte by byte
  (`SeaMlirExport.cc` shadow.mem.global.init): a store chain per global.
  Proposed: a `sea.memfill %m[%p], dense<bytes>` op (interpreter: writes
  the bytes), exported instead of the byte stores; the VC term is `MFILL`.
  Undef bytes are holes: the incoming memory shows through, and the
  exporter must make that incoming state the intended nondet state (as
  today). A symbolic lookup into the bytes is a later encoding choice.

## Reading through a term (SeaM rules)

A read `select(T, j)` reaching a term is decided once per term, not once
per cell:

- **skip**: j's object differs from d's object (different registered
  bases, both access addresses): `select(T, j) -> select(M, j)`. Premise:
  A-INB for the whole destination range (every cell of [d, d+n) is in d's
  object; n = 0 needs no access) and for j's access. Counted as
  `premise.a-inb`; the trail records it as a range premise.
- **skip (range)**: same object, `j - d` shown outside [0, n). Subtracting
  a common base is unconditional (mod 2^w); interval ordering is not:
  ordinary linear intervals are unsound for a wrapping range (4 bits:
  d=14, n=4, j=0 is a hit although j < d). ARM must use
  circular intervals (split ranges) here, or first establish a
  non-wrapping lifted interval (e.g. from A-INB: the range is inside one
  object, whose cells do not wrap) and say so in the premise.
- **hit**: `j - d` in [0, n) decided: MCOPY -> `select(Ms, s + (j - d))`
  (the read continues into the source: a translated read; an A-INB-based
  rewrite of that read needs the source range premise, and inside the
  undecided ite it describes a copied byte only under the guard: the
  premise carries the guard); MSET -> v;
  MHAVOC -> `select(H, j)`; MFILL -> the byte at a numeral `j - d`.
- **undecided**: `ite((j - d) <u n, <hit form>, select(M, j))`: one ite
  with one compare, not one per cell. When j or d is an ite, the compare
  is built over their leaf pairs (`Rules::buildIn`, as ROW's compare tree;
  `mem.in-push`): a pair decided in / out (A-INB across objects included)
  is true / false, only the other pairs stay `(j' - d') <u n`. An earlier
  version kept the whole compare when any pair was open, and the program
  rules then lost the pairs only A-INB decides
  (test/VC/seam/memop-in-leaves.mlir). MFILL with a symbolic `j - d`:
  decided later (an ite over the bytes or a lookup term); MFILL is step 2.

The hit / skip decisions reuse `decideEq`'s machinery (bases, ARM,
cancellation): `j - d` cancels a common base modulo 2^w without premises;
a different base needs A-INB. Every decision returns
its `seam::Decision` (rule + premises) for the rule trail (B.1) and
`--seam-check` (B.2).

Store-Map: a store-map summarizes numeral-offset stores of one object; a
range term over the same object ends the store-map run (as an unknown
store does today); a range term over another object is skipped by the
read before it reaches the store-map. [OPEN] whether MSET / MFILL with
numeral d and n should become store-map entries directly.

## Boundary and modes

The boundary is a semantic translation, not an optimization. `solverTerm` eliminates the terms by their defining equations
only: no A-INB, no ARM, no dependence on the rewriter having finished
(it may stop at its call limit, and `--seam-row-expand=0` deliberately
keeps undecided reads). It pushes each select down through stores
(`select(store(M,i,v), j) = ite(i = j, v, select(M, j))` only where M
contains a term), memory ites and the terms themselves, memoized by
(memory, index), until only solver operations remain. So a residual
`select(MCOPY(...), j)` is an ordinary case, not an error. This matters for
assurance: `--seam-check` sends both sides of a firing through
`solverTerm`; if the translation applied the optimizations, a wrong
firing could collapse to equal terms before the solver sees it.
`--seam-row-expand=0` affects only the optimizing rules, never the
translation. Model evaluation and replay go through the same translation.
A memory term in a position that is not read (an equality of memories, a
memory-valued check obligation) is refused with
`vc.memop-term-at-boundary`, by design; the checker reports such firings
as unexpressible, never valid.


- With `--seam`, the lowering does not run `sea-expand-memset` for VC
  input: symbolic lengths are encoded exactly, no bound, no assume. This
  changes the verified domain: executions
  with n > K, cut by the old `assume(n <= K)`, are now included, so an old
  unsat may become sat (a correction, not a regression). Input that was
  already expanded keeps its bounds and warnings.
  (Interpreter: executes symbolic lengths natively already.)
  The lowering pipeline depends on
  the encoding mode. Decided: keep `sea-expand-memset` for the off mode
  (array encoding has no range term) — yes, until the off mode has a
  lambda / range encoding of its own.
- Without `--seam`: unchanged (store chains; `sea-expand-memset`).

## What it does not fix (separate items)

- Contextual facts: `item_size in {1, 2}` and "the getter
  succeeded" live in the final accumulated assume; the dead 128-byte copies
  and the null alternatives survive. Scoped facts reaching ROW decisions
  are a separate design.
- Null as an object: `null + off` as an offset in a null object,
  so the null alternatives of a pointer compare by base. Separate design.
- The stride rule (`s*a + k = s*b + k'`, with premises on the full
  offsets) is a separate rule.

## Validation

- Read equations: independent checks of each term's defining read
  against a guarded-store expansion (independent small-width checks, now covered by a unit test), incl. overlap (Ms and Md the same array), n = 0 and
  maximal n, wraparound, different memory versions, memory ites,
  store-map interaction, and repeated reads of one MHAVOC (one H).
- `sea-expand-memset` stays as the reference for constant and bounded n:
  under the same explicit restriction n <= K, the term encoding and the
  expansion give equivalent queries (solver check per test program:
  B.3 style); n > K separately, reported as the intended domain change.
  The optimized rewriting is never the only reference.
- The boundary with optimization off or interrupted (row-expand=0, a low
  rewrite budget): same verdicts. Cross-object skips under their range
  premises, with a satisfiable-premise check. fuzz-vc (oracle) with
  memset / memmove / memhavoc of constant and symbolic length under
  `--seam`, fuzz-seam modes with copies between and within objects
  (overlap, zero length, symbolic n).
- Rule trail + `--seam-check` cover each skip / hit with its premise.
- Structural milestone: one range predicate per visited (copy, read)
  pair, shared by (memory version, index). Not a promise of a fixed DAG
  size: a read can continue into both the old and the source history.
- Empirical targets on array_list_swap (not guarantees; the native
  comparison also differs in placement, GSA, COI, simplification): DAG
  toward SeaHorn's lambda query (1,145 nodes by `qshape.py`; ours 5,244
  decided-only, 73,902 full), Yices at or below 17 s. Report the actual
  delta before deciding whether contextual facts are needed. Then the
  vcc / array-crafted bench: no verdict change within the old domain;
  every change explained (n > K).

## Order

1. MCOPY / MSET / MHAVOC terms, the read rules, `solverTerm`; `--seam`
   skips `sea-expand-memset`. Tests, array_list_swap acceptance.
2. `sea.memfill` op, exporter change in SeaHorn, MFILL term.
3. Word layouts.

## Step 1 implementation

Where: the operators `mcopy` / `mset` / `mhavoc` in `ExprOpArray.hh`; the
read equations and the boundary in `include/sea/Expr/MemOps.hh`
(`memopRead`, `MemOpTranslator`); the read rules in `SeaMRules.hh`
(`rowMemOp`, `decideIn`, `rangeInBounds`, `accessRanges`); the encoding in
`lib/VC/OpSem.cpp` (`execRangeTerm`; `solverTerm`, `firingObligation`,
`firingPremise`); `buildVCLoweringPipeline(..., expandMemSet)` (sea-vc
passes `!seam`). Tests: `unittests/MemOps` (test/Expr/memops.test),
test/VC/seam/memops-terms.mlir, the test/VC/memops corpus (EXPECT-SEAM
lines where the --seam verdict differs: the n > K programs).

- Encoding: with `--seam`, every memset / memmove / memhavoc of a memory
  with one-unit cells is a term, constant length included (the 128-byte
  copies of array_list_swap are one term each); d, s, n are normalized
  first, and the destination (and memmove source) range is counted as an
  A-INB access (`rangeInBounds`: n = 0, or n <=u size and off <=u size - n;
  a numeral n that does not fit is a static violation, as for a load).
  Word memories keep the whole-cell constant path; since `sea-expand-memset`
  (which also folded lengths computed from constants) no longer runs under
  `--seam`, that path accepts a length that normalizes to a numeral.
- Rules (`decideIn`, per leaf pair of the ites of j and d, as decideEq):
  `mem.empty` (n's range is [0, 0]); `cmp.diff-base` (pointer views into
  two registered objects of nonzero size: skip, A-INB); `mem.in-exact`
  (j - d a numeral: the same term, numerals, cancellation) and `arm.range`
  (one ARM entry each, same base or both absolute: x - y in an integer
  interval), both followed by the circular test (`circularIn`: the residues
  of the interval form an arc; an arc that passes 2^w - 1 is undecided), n
  as a numeral or a nonnegative absolute ARM interval. Counters
  `row.<op>-skip`, `row.<op>-hit`, `row.<op>` (undecided: one ite, one
  compare). Cross-object skips: by pointer views (`cmp.diff-base`), and by
  ARM for a read address without a view whose provenances are all other
  objects than d's (`arm.skip`, A-INB; d needs a view, so that its range
  premise can be stated). A split of the read over an ite index whose
  branches decide was tried and dropped (hash_iter_begin: DAG 15.7k ->
  21.6k, z3 29 -> 41 s). R-LEAVES (SeaM-mixed-width.md) splits only when
  a write gives every leaf read, decided down the whole memory (hash_iter_begin: no
  firing, the query is unchanged).
- Boundary: `MemOpTranslator` as specified (memo by node and by (memory,
  index); store-maps expanded first); counted `memop.translated`. The
  query fails with `vc.memop-term-at-boundary` on a refusal; replay treats
  a refused value as having no model value.

Deviations from the text above (each with why):

1. The guard of a translated read is not added to the premise of later
   A-INB firings on it. `firingPremise` states `inBounds(s + k)` for the
   read `select(Ms, s + k)` in the hit form, without `k <u n =>`. The
   checked obligation `inBounds(s + k) => lhs = rhs` is stronger than the
   guarded one, never weaker, and the composition still holds under A-INB:
   wherever the read's value matters (guard true) A-INB of the source range
   gives `inBounds(s + k)`; where it does not (guard false) the value is
   unused. So no unsound firing can pass, and the bounds job (which checks
   the source and destination ranges) still discharges what is used.
   Tracking the guard per translated index (the index is normalized after
   the rule fires) was not worth the machinery. [HYP] revisit if a firing
   on a translated read is ever reported invalid only for guard-false
   models.
2. Memory-valued firings over a term (a store rule whose memory holds a
   term) are not refused by the checker: `firingObligation` compares the
   two memories at a fresh index `seam-check.cell` (extensionality: the
   arrays differ iff some cell does), so the boundary sees reads only. The
   boundary itself still refuses a term that is not read; the checker
   reports such a firing unexpressible, as specified.
3. Constant-length MSET / MCOPY are terms, not store-map entries ([OPEN]
   above stays open): a term ends the store-map run below it.
4. Test hooks: `sea-vc --seam-rule-limit=N` (hidden; the rewriter's
   maxRuleCalls) to run the boundary after an interrupted rewrite; the
   corpus driver test/VC/memops/Inputs/run-memops.py gained `EXPECT-SEAM:`
   (the --seam expectation of a program where it differs from plain).

Results:

- Unit test (4-bit addresses, Yices): every read equation equals the
  guarded-store expansion (overlap, n = 0, n = 15, wraparound, versions,
  ites, stores and nested terms over a term); one H per mhavoc; the rules
  and store-map interaction exact under their premises (the cross-object
  skip is not valid without the range premise, and its premise is
  satisfiable); the boundary with row-expand off and after an abort.
  A wrong membership test (`<=u`) fails 21 checks.
- test/VC/memops (17 files) and test/VC/seam: same verdict under `--seam`,
  `--seam-row-expand=0` and `--seam-rule-limit=3` (z3, Yices), except three
  test/VC/seam programs that leave their objects (arm-inb-symbolic ov,
  inb-ptrmem, inb-ptrmemmove: --seam unsat is conditional on A-INB; an
  interrupted rewrite gives the unconditional plain answer, sat, replayed);
  `--seam-check` on all of them: no invalid, unknown or unexpressible
  firing.
- Equivalence (utils/memops-equiv.py, plain vs --seam): 74 programs
  equivalent; 4 differ, all n > K (memset-over-bound and memmove-over-bound,
  over_bound and edge_17: plain unsat, --seam sat, as the interpreter); with
  an explicit assume(n <= 16) added to those programs all are equivalent.
  memhavoc symbolic_len: plain refuses (vc.mem-op-symbolic-length), --seam
  unsat.
- array_list_swap (`--lower --widen-memory=8 --seam`, Release; before =
  HEAD 0320b0e): `vc.dag-size` 75,166 -> 2,363 (ite 52,548 -> 415, select
  308 -> 130, bvult 21 -> 131; 126 undecided mcopy reads, no
  `premise.a-inb`); vc-time 2.4 -> 0.04 s; Yices 282 s -> 8.4-10.1 s (the
  off mode: 17 s; SeaHorn's lambdas 12.8 s); Bitwuzla timeout (300 s) ->
  8.8-10.9 s; z3 timeout -> timeout. `--seam-check`: 975 firings valid
  (Yices); z3: 963 valid, 11 unknown (timeouts), 0 invalid.
- vcc-small: no
  verdict change; vcc-seam-yices 10.2 -> 9.9 s; vcc-seam-z3 9.2 -> 35.6 s,
  all of it hash_iter_begin (3.0 -> 29.7 s; Yices 2.6 -> 2.0 s). Cause: its
  reads index through `ite(c, null + o, obj.9 + o)` (the null alternative
  of a pointer) into a memset / memcpy of obj.5; the null leaf is an
  absolute address that no rule separates from obj.5, so the read stays one
  undecided `(j - d) <u n` ite per term where the store chain had per-cell
  equalities z3 handles better (DAG 11.2k -> 15.7k). The fix belongs to
  "Null as an object" (above), or to using the layout (obj.k = numeral, a
  side condition of the VC) as a stated premise; not done here.
