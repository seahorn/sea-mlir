# SeaM: object-aware memory simplification in the VC

Status: the rewriter and `sea-vc --seam` integration, address range maps,
Store-Map, and byte-memory ranged terms (`SeaM-memops.md`) are implemented.

## Goal

Implement Xiang Zhou's MASc work (SeaM, 2022) in sea-mlir. While the VC is
encoded, chains of stores become `ite`s with many simplifications. The
rewriter keeps track of *objects*, not only addresses. It uses the fact
that an offset into one object never (logically) aliases an offset into
another. It also compresses writes to the same object (Store-Map).

## Sources, and how much to trust each

| Source | Use it for | Trust |
|---|---|---|
| [Zhou's thesis](https://uwspace.uwaterloo.ca/handle/10012/18953) | T_M (Ch. 4), ARM (Ch. 5), Store-Map (Ch. 6), rewriter (Ch. 7), evaluation (Ch. 8) | primary design source |
| Historical SeaHorn implementation (`seam-dev14`), [SeaHorn](https://github.com/seahorn/seahorn) | rewriter behavior and measured rule order | low confidence: implementation reference, not an independent specification; inspect each rule when porting |
| [SeaHorn](https://github.com/seahorn/seahorn), `include/seahorn/Expr/ExprRewriter.hh` | rewriter framework | implementation reference |
| [Z3](https://github.com/Z3Prover/z3), `src/ast/rewriter/rewriter_def.h`, `rewriter_types.h` | rewrite behavior used as a comparison when designing ExprRewriter | implementation reference |

Re-imagine the rules rather than porting them. The mix and order of
simplifications affect performance; the measured SeaHorn order is recorded
below as a baseline for comparison.

## Meaning of SeaM terms, and what an optimization is

A SeaM term means what its translation to SMT means. The translation (the
solver boundary, `SeaOpSem::solverTerm`) is semantics-only: every SeaM
operator (store-map, the memory-operation terms of SeaM-memops.md, ...)
is eliminated by its defining equations, with no assumption and no
optimization, so every SeaM term converts to SMT whatever the rewriter did
or did not do. Optimizations are SeaM -> SeaM steps (the rewriter's rules).
An optimization is correct iff, for every firing t -> t',

    assumptions  =>  SMT(t) = SMT(t')

where the assumptions are the explicit, named ones the firing states
(A-SEP, A-NULL, A-NOWRAP, ...; see "Soundness stance"). This is the proof obligation for
each rule schema and what `--seam-check` checks per firing. It is also why
the translation must not apply optimizations: a
wrong firing could otherwise translate to equal terms and pass the check.

The translation also ends the pointer sort (PtrSort.md, "The boundary"):
`ptr(w)` becomes `bv(w)`, the markers their argument, `PTR_EQ` an `EQ`,
`PTR_DIFF` a `bvsub`, PTR_INDEX / OFFSET their arithmetic (SeaM's
normalized sum). That erasure is a function on terms (no assumption), so
the proof obligation above is unchanged: `SMT(t)` is the erased term.

## Soundness stance

Conditional soundness (Conway,
Dams, Namjoshi, Barrett, SAS 2008). Rules may rely on assumptions we do not
model in the VC, as long as each one is checked separately or stated.

Terms. An access address that normalizes to `obj.k + off` (a pointer into
object k: `off` mentions no registered base) is an access *through
provenance k*. The syntactic view is deliberate: `obj.a + (obj.b - obj.a)`
from integer casts is not a pointer into a, so it has no view, no rule
decides anything about it and no premise is used for it. An offset whose
value is only known through memory (a loaded integer) has a view. The
fat-pointer `sea.ptr.dereferenceable` checks are about provenance and do not
cover from_int pointers.

The encoding of `sea-vc --seam` rests on three named premises about the
program (v0.2.0; before, on the one premise A-INB):

- **A-SEP (separation):** in one execution, no memory cell is accessed
  through two different provenances. It is stated pairwise, for every pair of
  accesses (write / write and write / read). So a store through a and a read
  through b (a != b) touch different cells, whatever their offsets and sizes:
  `cmp.diff-base`, `arm.skip` across bases, R-SKIP, the range tests of memory
  operations across objects. WOW-COMMUTE / WOSMAP-COMMUTE need the write /
  write pairs: an execution that writes a cell through b, then through a,
  and reads it through a sees a's value, and the commuted term would put b's
  write on top. The weaker "a write never reaches another object" is not
  enough: a store through a past its end and a read through b before its
  start can both land in the same cell of a gap between the objects.
- **A-NULL:** no access of positive width (no range of positive length) is
  made through an object of size 0 (null is one, SeaM-null.md; zero-sized
  allocations). So an address alternative in such an object is dead
  (`addr.prune-size0`), and a ROW pair with such a side is vacuous
  (`cmp.size0-vacuous`). When the other side is a view of another base, the
  pair is two provenances and A-SEP decides it already; the decision then
  names A-SEP. A-NULL is needed only for a pair through one size-0 base with
  symbolic offsets, a side without a view, and the pruning.
- **A-NOWRAP:** an access address `obj.k + off`, read as an integer with
  `off` signed, lies in `[0, 2^n)`: base plus offset does not wrap. The
  stride rules use it for a byte offset K != 0 (R-STRIDE, `--seam-stride`,
  only in the K window `b + size - 2^n <= K <= b` where in-bounds gives the
  same, PtrIndex.md section 0c; the Ackermann stride relaxation for every K).
  ARM does not need it: offsets are signed w-bit integers, interval addition
  checks the overflow flags and an overflowing bound gives top, so every
  interval claims only "the value is the base plus some integer in [l, h]",
  true of the bit-vector arithmetic itself
  (`include/sea/Expr/AddrRangeMap.hh`, header comment). Two intervals of one
  base (or two numbers) that do not meet are therefore different with no
  assumption; the tests check that by evaluation at 8 bits (wraparound
  included).

**A-INB (in bounds)** is a property, not a mode of the encoding: every access
through provenance k lies in object k (`off + unit <= size_k`). With
placement's disjoint objects (`--seam` rejects overlapping or equally named
segments; placement refuses an object that wraps the space), A-INB implies
all three premises. A bounds check of the program discharges A-INB, and with
it the premises (below). Each premise is weaker than A-INB: a program that
overflows into the padding after an object breaks A-INB and keeps A-NULL,
and keeps A-SEP unless an access through another provenance reaches the
same padding cell.

**A-OWN (ownership)**, opt-in (`sea-vc --ack-own`, Ackermann.md, "The
ownership drop"): no access through one object touches another object's
cells, reads included. A-SEP does not imply it (two reads past their
objects' ends can meet in a gap cell; A-SEP is about pairs with a write);
A-INB of every access does. No SeaM rule uses it; the Ackermann lowering
uses it, with A-INB of one read, to drop the constraint of two reads of
different objects. This is no premise of an unsat answer (the drop only adds
models); it matters for sat answers and is not on the `assume.*` ledger
(counters `vc.ackermann-own-*`).

The premises are used only by the compares of a ROW step (store address
against read address), by access addresses (the pruning) and by the commutes
of two stores, never by a compare in the program: a compare is not an
access, and `a + 16 == b` (one past the end of a against the next object) is
legal C. Objects of size 0 are never "different" by base in a program compare
(in the SeaHorn layout they share a base with a neighbour). No rule replaces
`obj.k` by its placed numeral inside SeaM, so an access address keeps its
provenance; a step that substituted layout numerals into access addresses
before the ROW compares would lose it (the layout rules `cmp.layout` and
`mem.in-layout` run only without the premises).

**ARM and provenance.** An ARM entry (b, I) of an access address says: some
leaf of the term has provenance b (the base of the bvadd under the ites that
select it) and offset in I. A-SEP separates two entries of different bases;
an entry of a size-0 object is dropped before the test (`cmp.size0-vacuous`:
A-SEP when the maps are disjoint with every base its own provenance, else
A-NULL). The in-bounds check of the access (`Rules::inBounds`, the static
report below) covers the addresses ARM reads and the view does not (a sum
over an ite with many leaves): `armOutOfBounds` (an entry whose interval is
wholly outside its object or an object smaller than the access) and
`armInBounds` (the per-leaf condition, counted in `seam.premises
conditions=`). A part ARM does not read as a pointer (an absolute number: a
numeral, a loaded value) may be anywhere and is never separated from a base.

Every rule names the premises it uses (`// uses: A-SEP`). A rule with no
named premise must be valid over plain bit-vectors and arrays. Every
decision the rules make carries the premises it used, for all the leaf
pairs it looked at, not the first only (`seam::Decision`, SeaMRules.hh);
`seam.rule.premise.a-sep`, `-a-null` and `-a-nowrap` count the rewrites of a
run that depend on each. Counterexamples are wanted only if they satisfy all
premises; checks for the premises themselves stay in the program as
assertions.

## The premises: assumed and reported

`sea-vc --seam` does not discharge its premises. The answer is conditional:
an unsat holds for the executions that satisfy A-SEP, A-NULL and A-NOWRAP,
and the output says so on every `--seam` run:

    assume.a-sep: seam=<n> [drop-fresh-memhavoc=<m>]
    assume.a-null: seam=<n>
    assume.a-nowrap: seam=<n>
    seam.premises: assumed accesses=<n> conditions=<m>

(`seam=` = the rewrites that rest on the premise, 0: the answer does not use
it; `accesses` = the program accesses the premises are about; `conditions` =
accesses whose in-bounds condition is not constant, what a bounds check
would check. `seam.premises: off` for `--seam-premises=0`, which assumes
nothing, and `off (fallback: ...)`, below.) A sat is replayed as always. A
sat that needs an out-of-object access does not replay; sea-vc then runs the
abstract program (kept from before the lowering; every access checked
against its object) with the model's nondet values. If that run leaves an
object, sea-vc prints `seam.inb-violation: <message>` and
`seam.inb-violation-at: <loc>`, reports `vc.seam-inb-spurious` and exits 4
(inconclusive; the program has a memory-safety error). This is the
spurious-counterexample case a CEGAR loop would refine on. Otherwise exit 2
as before. The check is of bounds (A-INB), not of separation: a run that
leaves its object only into a gap keeps A-NULL, and A-SEP unless another
provenance reaches the same cell, and a VC bug on such a run
would be labelled spurious too (a separation check of the run, an owner per
cell, is a follow-up). The run must be the model's execution: it gets the
model's nondet values, and nondet memory cells are not given (the model has
addresses, not object offsets), so a run that reads one stops and decides
nothing (`seam.inb-violation: undetermined (...)`, exit 2 stays). Mapping
model cells to object offsets would widen it (TODO).

Using the A-INB-optimized encoding to prove A-INB is circular. Two attempts
at `--seam-check-inb` were removed after exposing the issue: every encoding
that can answer the query rewrites loads under A-INB. A load also feeds
assumptions, branch conditions and later addresses. A predicate "some
access leaves its object" built from that encoding can therefore drop the
violating execution
(`inb-circular.mlir`: write 7 through `a + n`, read `b`, assume the read is 7,
assert it is 0; plain is sat, the encoding under A-INB reads 0 and has no
model). Cutting the predicate to the first violation and to the side
conditions before it repaired some shapes, and a second encoding without A-INB
repaired others, but a pointer stored in memory and reloaded has a view, hence
an in-bounds condition, only in the encoding under A-INB (`inb-ptrmem.mlir`):
each version still gave a wrong unsat labelled as checked. A sound discharge
needs the instrumentation below, not a better predicate.

Static out-of-object accesses (v0.2.0: reported, no fallback). An access
whose every address alternative is outside its object (a numeral offset
outside it, an object smaller than the access or of size 0, an ARM entry
wholly outside it) is flagged by `Rules::inBounds` while encoding. sea-vc
classifies each one by where it lands in the placement
(`Rules::outOfObjectSpans`, then pairwise over all of them,
`SeaOpSem::classifyOutOfObject`), worst first:

- `object`: a span meets another object (A-SEP fails if the access
  executes);
- `shared`: a span lands in a gap cell that an out-of-object access through
  another provenance also reaches, one of the two a write (A-SEP fails if
  both execute; a load and a load do not break it). A-SEP is pairwise, so
  "lands in no object" is not enough;
- `unknown`: a symbolic offset or length, an ARM-only leaf, or a gap span
  while some access of unknown landing exists (it may reach that cell);
- `null`: through an object of size 0 (A-NULL fails if it executes);
- `gap`: cells no other out-of-object access reaches through another
  provenance; in-bounds accesses never reach them, so A-SEP and A-NULL hold
  and only A-INB fails.

The pairs cover the accesses the encoding flagged; an access with a symbolic
offset that leaves its object is not flagged and not paired (it is covered by
A-SEP as an assumption, like every access). Different memories are not told
apart (conservative: a pair in two memories is reported shared). It warns
`vc.seam-out-of-object` (saying, per class present, which premise breaks
when what executes), names the first 8 as `seam.out-of-object-at: <loc>
<op>: <reason> lands=<class> [with=<loc>]` (`with`: the other access of a
shared pair) and prints

    seam.out-of-object: accesses=<n> object=<a> shared=<s> unknown=<u> null=<z> gap=<g>

and keeps the premises. The check is path independent (an unreachable leaf
also triggers it, `arm-unreach.mlir`; the speculative random-access paths of
an STL algorithm trigger it in programs that never execute them), and sees
only what is visible while encoding (`inb-ptrmem-static.mlir` is caught,
`inb-ptrmem.mlir` with a symbolic `n` is not). So an unsat with `object`,
`shared`, `unknown` or `null` above 0 holds only if those accesses do not
execute; one with only `gap` accesses rests on its premises as any other
(`out-of-object-shared.mlir` shows both).
`--seam-out-of-object=fallback` restores the behaviour before v0.2.0: the
run starts again without the premises (`seam.premises: off (fallback: an
access leaves its object)`), the plain answer.

Discharging the premises is a separate memory-safety job: instrument the
program with a bounds check before every access (the object its pointer came
from, as SeaHorn's `sea fpf` does), and verify the instrumented program; an
unsat of the instrumented job (A-INB holds) together with an unsat of the
original one gives the unconditional claim. A-NULL alone is a
null-dereference check when null is the only object of size 0 (zero-sized
allocations need their own check), cheaper and more common. `--seam-check`
states every premise firing as the in-bounds conditions of its accesses;
per-premise statements (A-SEP, A-NULL, A-NOWRAP each on its own) are an open
follow-up. Future work: a sea-mlir pass could check
using only the object information it knows (the registry, allocation ops,
and provenance of pointers stored in memory). It could minimize checks by
omitting accesses the rules already show are in bounds and reusing one check
for equal accesses. The premise provenance (`seam::Decision`,
`premise.a-sep` / `-a-null` / `-a-nowrap`) tells which jobs need it.

## Where it lives

```
sea-mlir pipeline (MLIR -> MLIR)       ...  sea-place-static  ...  BMC form
                                                 |
                                                 v  (new) object base markers survive placement
VC encoding (OpSem / VCGen, Expr)      load / store encoding  --->  SeaM rewriter (per access, shared cache)
                                                                     |
                                                                     v
                                                                   solver
```

1. **Object provenance survives placement.** Today `sea-place-static`
   replaces each object by a numeral address, and `sccp` folds
   `base + off` into one numeral. That loses which object it is.
   Decision: `sea.segment` gets a result, the object's base address
   (`%b = sea.segment "alloca.3" base 3221225456 size 16 align 8 kind
   "stack" : !sea.addr<64>`). Placement replaces an object's uses with
   `%b` (plus `addr.add` for offsets) instead of a constant. The op has no
   folder, so `sccp` cannot erase the provenance; the interpreter
   evaluates it to its base. One op stays the single record of where an
   object is, and no second marker op is needed. Placement is still an
   MLIR -> MLIR pass; VCGen only encodes.
2. **The VC keeps bases symbolic.** Each object base becomes a named
   constant `obj.k`, with a side equality `obj.k = 0x1000`. The concrete
   model is unchanged: `ptr.to_int`, alignment and replay all work as now.
   VCGen hands the rewriter a **base registry**: the set of `obj.k`
   constants, each with its allocation order and size. The rewriter never
   parses names (the branch matched `sea.obj*` by name).
3. **Pointer view.** No new Expr sort: addresses stay bit-vectors. The
   rewriter reads a term as a pointer when it normalizes to
   `bvadd(obj.k, off)` with `obj.k` in the registry. A pointer loaded from
   memory or made by `ptr.from_int` has no base. Rules that need a base do
   not fire on it, and the plain ROW `ite` is used.
4. **When it runs.** This is eager and per access, as in the thesis and
   the branch. Each store is normalized (and WOW-rewritten when Store-Map
   is on) when it is encoded. Each load builds `select(mem, ptr)` and
   rewrites it at once. One rewriter cache lives for the whole VC, so
   memory terms shared by many loads are rewritten once (Thm 5).
5. **Depends on the ite join encoding.** A memory merged at a join must be
   `ite(e, m1, m2)` so that a read can be pushed through it. Under
   `--vc-join=eq` the merged memory is a fresh constant and ROW stops
   there. `--vc-join=ite` is the default since f638f22. SeaM requires it
   (error otherwise).

## Rewriter framework (port)

Port `ExprRewriter` into `include/sea/Expr/ExprRewriter.hh`. The shape
stays: a `Config` template with `shouldRewrite` / `doRewrite` /
`onAfterRewrite`; statuses `RW_DONE`, `RW_1`, `RW_2`, `RW_FULL`,
`RW_SKIP`; an iterative traversal with a frame stack and a result stack;
a cache from each node to its fully rewritten form. Changes, after
comparing with z3:

- **Nested-rewrite caching.** The change in
  Xiang's rewriter was made for empirical reasons, and it is about
  *nested* rewrites: one rewrite produces a term whose rewriting it
  enables (a ~> b ~> c, e.g. ROW gives `ite(i = j, v, select(a, j))`; the
  compare resolves; that enables the ITE rule; that exposes the next
  `select`). Without caching the start of such a chain (a |-> c), the
  same chains were rewritten again from every place that reached `a`, and
  rewriting timed out. The thesis calls this multi-step caching (Ch. 7,
  Alg. 4-5, Thm 5; the trigger was R-ABORT). Port it faithfully:
  - The branch marks the frame that started a chain (`m_rewriting`) and,
    when the chain ends (`RW_DONE` / `RW_SKIP`), caches start |-> final
    (`processTopFrame`). It re-pushes a reference to a frame it has just
    popped (undefined behaviour that happens to work): fix that, keep the
    behaviour.
  - z3 has a similar mechanism (`rewriter_def.h` `process_app`: the frame
    stays in state `REWRITE_BUILTIN` and caches t |-> final when the
    follow-up rewrite returns), but caches only shared terms (`rewriter.h`
    `must_cache`: ref_count > 1, not the root). Whether that would have
    been enough for SeaM is not known; do not switch to z3's policy
    without the test below showing it is.
  - The branch also caches every `select`, `store` and `ite` regardless of
    sharing (`utils::shouldCache`). [HYP] This helps hash-consed rebuilds
    (both `ite` arms of Fig. 7.1 rebuild the same `select(m_a, p)`); keep
    it, and measure it separately.
- **The test that makes it solid (write it first, before the rules).**
  The original test was probably not kept. It must exercise nested
  rewrites reached from several places:
  - Fig. 7.1 scaled: `m_{k+1} = ite(c_k, store(m_k, p_k, v_k),
    store(m_k, q_k, w_k))` for k = 0..N, then rewrite `select(m_N, r)`,
    with addresses chosen so that every compare resolves (each ROW step
    is a nested rewrite chain). Without caching chain starts the work is
    exponential in N; with it, linear.
  - The R-ABORT shape once Store-Map exists (a store-map reverted to a
    store chain and re-read).
  - Assert a bound on rule applications (a counter), for example at most
    c*N for N = 40, so a regression fails deterministically instead of
    timing out. Also run with chain-start caching disabled and check the
    count explodes, so the test is known to detect the problem.
- **Cache ownership.** Keys are `Expr`, not `ENode*` with manual
  `Ref`/`Deref`. Store-Map's side map is owned by the rewriter (see below).
- **Depth semantics** are the thesis' (Alg. 2 line 25) and the branch's,
  not z3's: the follow-up frame of `RW_n` has depth n, so rules apply at the
  result and n levels below it; `RW_FULL` is unbounded. (A first port used
  z3's reading, one level less; the tests showed it loses simplifications the
  branch's rules rely on.) The cache is depth aware: an entry records how
  many levels were rewritten and a deeper visit does not take it (the thesis
  caches a bounded visit as a full one; a shared term seen at the edge of a
  follow-up then stayed unreduced for later full visits). A chain's entry
  takes the depth of the start frame: `RW_n` is the rule's promise that n
  levels suffice, given normal kids. The config must therefore visit every
  operator on a path to a rewritable term (STORE included), or the promise
  is false.
- **Constant compare** folds numerals modulo 2^width (`bv::bvnum` does not
  reduce; the backends do).
- **Statistics per rule** (counters, as the branch's `hybrid.*` stats) and
  an `--agent` friendly dump. These are needed for the order experiments.

## Word-sized memory

SeaM expects aligned word reads and writes wherever possible: the thesis
states T_M with `read-word`/`write-word` and byte versions and then works
with words only ("without loss of generality", §4 p.23), but does not say
that the gains depend on it. With byte memory every 4-byte value is
written and read back as 4 bytes, and the VC fills with byte reassembly
(`concat(extract(x,31,24), ..., extract(x,7,0))`). Xiang's array-crafted
jobs ran with `horn-bv2-word-size: 1` (byte memory). So SeaM should be
combined with either:
- `sea-widen-memory` (memories whose accesses are all aligned whole words
  become word memories; `sea-vc --widen-memory=k`), or
- a layout that maps aligned operations to word operations and keeps the
  others as bytes (parked step 6, ignore-alignment with checks).
Measurements report the widening used. Rules for extract/concat (byte
reassembly) are still needed for the accesses that stay bytes.

## Rules

T_M in our setting: `ptr(b, o)` is `bvadd(obj.k, o)`; `=p`, `<=p`, `+p`
are bit-vector ops on normalized pointers. Each rule lists its thesis
source and its assumptions. "Phase" is the planned order of work.

| Group | Rule | Source | Uses | Phase |
|---|---|---|---|---|
| Normalize | flatten n-ary `bvadd`, fold constants (no wrap check needed for equal-width modular folding) | SeaHorn arithmetic rule | none | 1 |
| Normalize | pull `bvadd` through `ite` for pointer terms (bounded, it is exponential) | SeaHorn pointer arithmetic config | none | 1 |
| Normalize | `a - a ~> 0`, `a - c ~> a + (-c)`, `ptr(b,o1) - ptr(b,o2) ~> o1 - o2` (added; thesis Fig 4.3 `-p`) | Fig 4.3 | none | 1 |
| Compare | `ptr(b,o1) =p ptr(b,o2)  ~>  o1 = o2` | Fig 4.2 | none | 1 |
| Compare | `ptr(b1,o1) =p ptr(b2,o2)  ~>  false`, b1 != b2, only the compare of a ROW step, objects of nonzero size | Fig 4.2, Ch 3 | A-SEP | 1 |
| Compare | constant folding, `a - b = a - c ~> b = c` | Fig 2.2 | none | 1 |
| ROW | `select(store(a,i,v), j) ~> ite(i = j, v, select(a,j))`, then HIT / SKIP when the compare resolves | Fig 4.6 | as Compare | 1 |
| ROW | `select(ite(c, m1, m2), j) ~> ite(c, select(m1,j), select(m2,j))` | Fig 2.6 | none | 1 |
| Compare | `ite(c,x,y) = w ~> ite(c, x = w, y = w)` (ITE-EQ), only when bounded and a leaf resolves | Fig 2.1 | as Compare | 1 |
| ite / Bool | `ite(true,..)`, `ite(c,a,a)`, `ite(a,true,false)`, `ite(a,b,ite(!a,c,d))`, double negation, negation into ite | Fig 2.1, 2.3 | none | 1 |
| ARM | `alpha(p) meet alpha(q) = bot  ~>  p != q` (interval per base, no ITE-PUSH); ROW compares only | Ch 5, Thm 4 | A-SEP (different bases); none for one base | 2, done |
| Store-Map | SMAP-NEW, SMAP-HIT, WOW-COMMUTE, WOSMAP-COMMUTE | Fig 6.4 | A-SEP (commute across objects, write / write) | 3, done |
| Store-Map | R-HIT, R-MISS, R-SKIP, R-ABORT | Fig 6.5 | A-SEP (R-SKIP) | 3, done |
| Ranged | read over memset-words / memcpy-words (+ HIT/SKIP) | Fig 4.5, 4.7 | A-SEP | 4 |
| Bit-vector | extract of extract / concat / numeral / full width; concat: flatten, fold numerals, merge adjacent extracts of one term (byte reassembly) | z3 simplify leftovers | none | 5 |
| Bit-vector | linear equality: cancel common addends (any term), numerals folded to one side, `a + c1 = a + c2`, `x = x`, `24 * x = 7` (low zero bits), canonical side order | SeaM normalization rules | none | 5 |
| Bit-vector | AC normalization: flatten, sort by creation id, fold numerals, idempotence (`bvand`, `bvor`, `and`, `or`), `x ^ x = 0`, `x & !x` | SeaM normalization rules | none | 5 |
| ite / Bool | Boolean ite to `and` / `or`, same condition in an arm, same value on two paths (`ite(c, a, ite(d, a, f)) ~> ite(c | d, a, f)`), `ite(!c, a, b) ~> ite(c, b, a)`, implication | SeaM normalization rules | none | 5 |

Notes:

- **Ranged ops** would let `sea-expand-memset` stop expanding
  constant-length memset/memmove into store chains, and handle symbolic
  lengths without the `n <= K` bound. That changes the pipeline contract
  (VCGen would encode `sea.memset` directly), so it is a later decision.
- **ARM as an analysis.** ARM is per term in the thesis (O(n^2) to
  build). Values in the VC come from SSA values, so the interval per base
  is computed once per Expr node and cached (`AddrRangeAnalysis`, iterative:
  the ite chains of a VC are deep). It feeds the compare rule as a last
  resort, after the syntactic checks, and only for the compare of a ROW step
  (`decidePair`, and `decideEq` when the leaf pairs exceed the bound):
  `arm.skip`. Never a program compare, though the same-base part would need
  no assumption. Differences from
  the thesis figures: top / bottom and the absolute entry as above; bottom
  absorbs add (Fig. 5.3 has `[a, b] +^ bot = [a, b]`, wrong for a sum of
  sets); a mutation switch for the tests.
- **Store-Map representation (implemented).** The Expr node
  `store-map(arr, base, ocl)` holds the insertion-ordered cons list
  (`smap-cons(offset, value, rest)` ... `smap-nil`, immutable, as in Fig 7.2);
  it types as the array sort of `arr` and means "`arr` with the cells
  `base + o_k` written with `v_k`", with no assumption (the offsets are
  numerals: distinct numerals are distinct addresses). The ordered map
  (offset -> latest value) is a rewriter-owned side table
  (`StoreMapTable`, `include/sea/Expr/StoreMap.hh`). Decisions:
  - *Key.* The table is keyed by the **list node**, not by the store-map
    node (thesis Ch. 7.2: store-map -> map). The map is a function of the
    list, so a store-map rebuilt over another array (WOSMAP-COMMUTE, a
    rewritten kid) keeps its map and no transfer hook is needed (the branch
    needed `onAfterRewrite` for it, and its manual Ref / Deref counted a node
    twice). Keys are owning Exprs.
  - *Insert.* The new list takes over the map of the old one (moved: the old
    list has none). An insert into a list without a map (a memory version
    extended twice: a join, or an old version) rebuilds it from the list
    (`smap.rebuild`); a lookup in a list without one scans it
    (`smap.scan`, the thesis' "linear search of the OCL"). A store of the value
    an offset already holds returns the store-map itself (`smap.noop`).
    Shadowed entries stay in the list (as in the thesis; no compaction);
    expansion and R-ABORT use the distinct offsets with their latest values.
  - *Order of the chain.* The head of a normal chain is the latest allocated
    object (the registry's `order`): WOW-COMMUTE moves a write below a write
    to a later object, WOSMAP-COMMUTE below a store-map of a later object, both
    only for pointers into different registered objects under A-SEP
    (`Rules::commutes`). A normal `a` has nothing later beneath, so
    SMAP-NEW's store-map is in order (`RW_DONE`). Barriers (a pointer without a
    view, a symbolic offset, a size-0 object) are never moved across or
    merged: the thesis' "temporal order of writes at symbolic addresses is
    kept".
  - *Rules and assumptions.* SMAP-NEW, SMAP-HIT, the collapse of an identical
    address, R-HIT and R-MISS use no assumption; WOW-COMMUTE, WOSMAP-COMMUTE
    and R-SKIP use A-SEP (and with `--seam-premises=0` do not fire: the result
    then holds over plain bit-vectors and arrays, tested). R-ABORT decides each
    of its entries with the compare of ROW, which uses A-SEP when the read
    address and the entry are in different registered objects
    (`cmp.diff-base`, cross-base `arm.skip`); an entry on the same base is
    decided with no assumption, and with `--seam-premises=0` none uses it. An
    A-SEP-dependent unsat can therefore also come from R-ABORT; the
    `row.smap-abort-skip` / `cmp.diff-base` counters show it.
  - *R-ABORT is one-shot,* as in SeaHorn's `revertSMapToIte`, not the
    thesis' peel of one entry per step: the read becomes the chain
    `ite(b + o_n = i, v_n, ... select(arr, i))` over the distinct entries,
    newest outermost, each entry a ROW step (the compare of `row()`, `Yes` ends
    the read, `No` drops the entry). The thesis' step `read(write(store-map(a, b,
    m \ o1), ptr(b, o1), v1), i)` needs a store in the rewriter's input and
    this config rewrites every store it meets (it would merge it back); the
    chain has no store at all (with `--seam-row-expand=0`, the unknown entries
    become a plain store chain that is final, `RW_DONE`: never visited, never
    merged again). Consequences: O(entries + list length) per aborted read; the
    ite chain is cached by its own frames, so chain-start caching does not
    matter for it unless a store-map has one open entry (the select is then a
    kid of the follow-up): the test covers both shapes.
  - *Where store-maps live.* In the memory values of the VC (that is the data
    structure: the next store updates one). They never reach a solver: every
    read over one is rewritten (no `select` over a store-map survives a
    rewrite), and the boundary `SeaOpSem::solverTerm` (the query, the A-INB check
    query, every model evaluation of the replay) expands any left (a rewrite
    aborted at `maxRuleCalls`; a memory term used as a value; with
    `--seam-row-expand=0`, an undecided read stays a `select` over a store
    over a store-map) into stores
    (`expr::StoreMapExpander`, iterative, memoized, counter `smap.expanded`,
    0 in the tests with the default options). A `store-map` term is not an SMT-LIB term; the backends do
    not know it.

## Order and mix (what was measured)

The configuration from the branch, to reproduce first, before changing
anything:

1. Store: normalize the pointer (flatten / fold `bvadd`, pull through
   `ite`). If Store-Map is on, run the WOW rewriter on the new `store`.
2. Load: normalize the pointer, build `select(mem, ptr)`, rewrite with
   one config that dispatches by operator: ite, compare (syntactic base
   check first, ARM last), Bool, ROW (store / ite / memset / memcpy /
   store-map), arithmetic. ROW returns `RW_2`: the new `ite` and its two
   children (the compare and the next `select`) are rewritten again. So
   the chain walks down the stores one at a time, and stops early when a
   compare resolves.
3. After each load the branch ran z3 simplify. The implementation applies
   its own rules here; z3 simplify remains a separate comparison variant.

The bit-vector and Boolean normalization rules in
`include/sea/Expr/BvRules.hh` were added to the same config for every term of the VC
(the side conditions and the query go through the rewriter in a final pass,
with the cache of the loads and stores), because z3 simplify still shrank our
queries 2-4x: byte reassembly, linear equalities, argument order, Boolean ite.
`--seam-simplify=0` runs the configuration above without them; `--simplify`
runs them without SeaM (plain VC). Each rule is valid over plain bit-vectors
(no assumption), tested one by one and on random terms against Yices. The
order of the arguments of the AC operators is the creation id of the terms
(`ENode::getId`: a factory counter, not an address, so the same in every run);
the emitted query is identical across runs (checked).

Open question: which alternative orders perform worse? Make the order
explicit, add per-rule counters and ablation flags, and compare against the
thesis measurements (ARM ablation: Table 8.3; Store-Map: Table 8.4, bAnd
Table 8.5).

## Validation

- **Rule soundness, mechanically.** Every rule instance `lhs ~> rhs` is
  checked by the solver with `sea-vc --seam-check`: `premises => lhs = rhs`
  (A-SEP / A-NULL / A-NOWRAP stated as the layout plus the in-bounds
  conditions of the compared accesses, A-INB, which implies them); see "Rule trail and per-firing check". A rule that needs an
  assumption it does not name is a bug.
- **Unit tests** per rule (doctest-like, or lit via `sea-expr-eval` if it
  grows a rewrite mode).
- **Differential.** VC with SeaM on/off must agree: fuzz-vc (three
  solvers), opsem tests, verify-c-common. Every sat must still replay with
  0 mismatches.
- **Assumption audit.** A sat under SeaM that disappears without it means
  an assumption was used that the program does not check. Report it as an
  error, not as a speedup.

## Rule trail and per-firing check

**Trail** (`include/sea/Expr/RewriteTrail.hh`; `sea-vc --seam-trail=<file>`,
`--seam-trace=<rule,...>`). A *firing* is one rule call of the rewriter
(`Config::doRewrite`) that changed its term: lhs is the term the rule was
given (kids already rewritten), rhs the term it returned. The hooks:
`ExprRewriter::processTopFrame` brackets each rule call (`trail->begin()` /
`trail->end(lhs, rhs)`); `RewriteStats::count` notes every counter a rule
bumps during the call (the rule itself and the reasons of the decisions it
rests on: `row.store-skip` with `cmp.diff-base` or `arm.skip`;
`premise.*` counters are dropped, they become the premise field);
`Rules::countPremises` notes the `Premise` bits (A-SEP, A-NULL, A-NOWRAP). The scratch counters
of ITE-EQ / the ROW compare tree are merged with `RewriteStats::merge`, so
the reasons of a resolved tree are in its firing and those of a discarded
attempt are not. SeaOpSem sets the context: the op being encoded (`load`,
`store`, with its location and access size in address units; `addr` for the
normalization of an access address, with no size: the address may contain
loads of other memories) or `side` / `query` for the final normalization.
Covered: the SeaM rules, BvRules, Store-Map, ROW steps, ARM decisions (as
reasons). Not a firing: the store-map expansion at the solver boundary.

Caching: a firing is recorded once, when the rule runs; a cache hit is
counted (`cache_hits` in the run's summary record), not recorded. A term a
hit returns is the rhs of the last firing of the chain that made it, so the
trail finds the rules behind any term (`utils/seam-trail.py --contains /
--produced`). Record format: JSON lines, one per firing (rules, premises,
op, loc, units, lhs / rhs node ids; `--seam-trail-text` adds the printed
terms), plus `run` and `summary` records; the term DAG, one record per node,
in `<file>.terms`. Off, the cost is a null test per rule call and per
counter bump (no allocation). On (hash_table_eq, 244k firings, Release,
`--lower --widen-memory=8 --seam --smt2`, median of 5): VC time 2.26 s off;
2.25 s with `--seam-trace` of a rule that never fires (trail active, nothing
written); 2.92 s (+29 %) writing the trail (63 MB trail + terms);
about 18 s (705 MB) with `--seam-trail-text` (printing is the cost: debugging
only).

**Per-firing check** (`sea-vc --seam-check[=N]`): every kept firing (or N,
evenly spaced) is a solver query `premise /\ lhs != rhs` (store-maps of both
sides expanded through `SeaOpSem::solverTerm`) on the run's solver, with
`--seam-check-timeout` (10 s; not for Yices). The premise
(`SeaOpSem::firingPremise`): `true` for a firing without premises (the rule
must be valid over plain bit-vectors and arrays, without the layout); for a
firing that uses A-SEP, A-NULL or A-NOWRAP, the layout (`obj.k = base`, a
fact of the VC) and `Rules::inBounds` of each access address the rule compares
(`Rules::accessAddresses`: the select index or store address, and the
address of the write below it, or every entry of the store-map below it), at
the access size of the context: A-INB of those accesses, a sufficient
premise (it implies the three). Stating A-SEP itself for a cross-object pair
would make the premise the conclusion (the two cells differ), and the check
would test only that the rule saw two provenances; the in-bounds form keeps
its teeth. This checks that the rule is a correct consequence of a
sufficient premise; it does not discharge one (that is the bounds-check
job). A premise that cannot be stated (a premise firing in an
`addr` context, a term that is not a select / store / wselect or a unit
overlap of two recorded access bytes, a statically violated
access, or a premise the solver finds unsatisfiable: checked first, so a vacuous premise never makes a firing valid) is `unexpressible` (warning `seam.check-unexpressible`), never valid.
Output `seam-check: firings= checked= valid= invalid= unknown=
unexpressible=` after the verdict, `seam-check-invalid:` (rule, op, location,
terms, SMT-LIB) per invalid firing; exit 5. `--seam-check-cost` adds
`seam-check-cost: id= rlimit= time=` per checked firing: the z3 rlimit
count of its query (a fresh solver each, so the count is the query's) and
its time, the measurement an rlimit is chosen from
(test/VC/seam/fuzz-check.test). `--seam-plant-bug` (hidden, test
only) makes `sub.const` or `row.store-hit` wrong; `test/VC/seam/check-planted.test`
shows the check catches both.

Results (z3, timeout 10 s per check): every program of
test/VC/seam under seven settings (default, --widen-memory=4,
--seam-store-map=0, --seam-row-expand=0, --seam-simplify=0, --seam-arm=0,
--seam-inb=0, now --seam-premises=0): 5204 firings, all valid. utils/fuzz-seam.py --seam-check
(Yices, seed 7, 15 programs per mode, every --seam variant): bases 1679,
any 2233, wide 6216, cells 32425, cells-violate 71930, circular 28993,
ptrmem 18789 firings, all valid. verify-c-common (`--lower
--widen-memory=8 --seam`), 10 jobs: 9 checked in full (6305 firings: 6275
valid, 30 unknown, all timeouts of add.pull-ite in array_list_erase, 0
invalid); hash_iter_next2 in full (about 45 min): 38103 firings,
38019 valid, 84 unknown (timeouts: 80 row.ite, 2 bool.and, 2 bv.ite-merge),
0 invalid. No invalid and no
unexpressible firing anywhere; the planted bugs are caught.

## Benchmarks

- verify-c-common (jobs, jobs2, jobs_unsafe), all three solvers. z3 is
  the one expected to gain (thesis: Arrays 3,269 s -> SeaM 341 s on z3;
  Yices about level, 451 vs 422 s; ARM gave 3x on Yices against SeaM
  without ARM).
- **The outsize-difference example: SV-COMP array-crafted.** 10
  programs (Table 8.4: pre-processing 233 s Lambdas, 102 s SeaM-linear,
  3 s SeaM), plus the bAnd case study taken from it (Fig 8.9, Table 8.5).
  The benchmark sources are in the public
  [`danblitzhou/verify-c-common` repository](https://github.com/danblitzhou/verify-c-common),
  branch `xiang-thesis-bench`, under `seahorn/jobs_bench`. They motivate
  evaluating Store-Map and rewriter caching.

## Phases

0. Rewriter framework port with the caching test (Fig. 7.1 scaled) as its
   first test; nothing else depends on it until that test holds.
1. Phase-1 rules + base markers through placement (`sea.segment` result)
   + base registry in VCGen. Measure on verify-c-common, mainly z3.
2. ARM.
3. Store-Map; thesis SV-COMP / bAnd benchmarks.
4. Ranged memset / memcpy terms (pipeline contract change).

## Design decisions

1. Outsize-difference example: SV-COMP array-crafted (and bAnd).
2. Only our rules; z3 simplify after loads is a later exploration.
3. Base marker: use `sea.segment` with a result (above).
4. All objects get a base from the start. SeaHorn turns all allocations
   into stack allocations, so apart from globals other kinds do not occur
   there; sea-mlir handles every placed object the same way.
