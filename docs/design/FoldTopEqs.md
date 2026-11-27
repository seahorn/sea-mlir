# Folding the top-level facts of the VC

`include/sea/VC/FoldTopEqs.h`, `lib/VC/FoldTopEqs.cpp`; `sea-vc
--vc-fold-top-eqs`, on by default where it applies (section Default).
Tests: `test/VC/fold-top-eqs/`.

Status: implemented, exact, on by default; `--vc-fold-top-eqs=false` is the
A/B baseline (`bench/configs/no-fold-top-eqs.yaml`).

## Why

A harness assumes values of its input state: `assume(ws->n == 80)`. In the
VC after SeaM's read-over-write, such an assume in the entry block is a
top-level conjunct `T = c`, where T reads the initial memory (a read, a
concat of the reads of a wide load) and c is a numeral. The program reads
the same cells again later, mostly after stores to other cells of the same
object. `sea-assume-fold` replaces the uses of the assumed value in MLIR,
but only of loads of the initial memory version: a load of a later memory
version is a different SSA value. After read-over-write in the VC, that
load is the same read term as the assumed one (Expr is hash-consed), and
nothing substitutes c for it.

The reads that stay symbolic cost the Ackermann lowering: their indices
and values enter pairs the deciders cannot settle. In the firedancer step
jobs (`--lower --seam --widen-memory=4`), the substitution cut the
Ackermann constraints from 1366-3740 to 146-436 (table below).

The solver's own variable elimination (z3's `solve-eqs`) would substitute
the facts too, but only after the lowering decided its pairs, and the
goal is a query that leaves it no work: no top-level `x = c` with x a
constant, and no pair whose guard becomes decidable after a substitution.

## What it does

Two steps.

**1. The fold** (after the rewriting of the query, before the solver
boundary and the Ackermann lowering; `TopEqFolder`). The query is a
conjunction (`mkQuery`: the side conditions and the negated
guarantees). Its conjuncts, nested ANDs flattened, are the top-level
facts; a conjunct under an implication is not one (an assume in a block
other than the entry is `guard -> c`). The facts:

- term: `T = c`, either side, c a bit-vector numeral, T not a numeral;
- split: for `concat(a, b, ..) = c` also each part, `a = hi(c)`,
  `b = lo(c)`, recursively (a part's width from the type checker; a part
  of unknown width is skipped, `skipped-untyped`);
- bool: a Boolean constant as a conjunct, `b` (b := true) or `not b`
  (b := false): the guard of the exit block, Boolean nondets.

Skipped and counted: `bvand(x, m) = c` with a numeral mask (`skipped-masked`):
it fixes the bits of x under m only; T, or a concat part, that mentions
an object (`skipped-base`, section Pointer observations).

Every occurrence of a fact's T in the other conjuncts becomes c. The
defining conjunct stays, rebuilt with its own T kept (`T = c` never
becomes `c = c`); the other facts are substituted inside it too. The
rebuilt path from the equation to its keys includes a concat skipped for
an object (section Pointer observations): substituted as a term, it would
replace its parts inside their own fact, `concat(p, a) = c` would become
`concat(hi(c), a) = c`, and `p = hi(c)` would leave the query while p is
replaced everywhere else. Then the
whole query is simplified again (the same rewriter, so ites, read guards
and comparisons fold), concats whose parts are numerals and ites on one
condition become ites of concats (`concat-ite`, below), and the round
repeats until the query does not change, collecting new facts each round
(`rounds`). A bound of 64 rounds stops it with the warning
`vc.fold-top-eqs-rounds` (exact either way). After the last round, a fact
that lost its defining conjunct is a conjunct again (`re-added`, section
Step 1's trail; normally none).

### The concat-ite hoist

`concat(p1, .., pn)` whose parts are numerals or ites on one condition c
becomes `ite(c, concat(then-parts), concat(else-parts))`, again on the
arms. The Ackermann decider takes the leaves of ites, not of concats: a
wide load of cells that conditional stores set to numerals becomes an ite
of numerals, and an index made of it is decided leaf by leaf. Exact:
concat is a function of its parts. The hoist is memoized per concat node
(the key held: the arm concats are made by the hoist, and a freed node's
address can come back as another node), so its work is linear in the
distinct concats it makes; `concat-ite` counts those. `concat-ite` and
`concat-ite-refused` are summed over the fold rounds: a concat refused in one
round and still in the query is counted again.

`--vc-fold-concat-ite` selects where it fires:

- `numeral` (default): only when every leaf of the result is a concat of
  numerals and the hoist makes no more ites than the parts hold along
  their ite spines (no growth). A top concat it refuses is counted
  (`concat-ite-refused`).
- `all`: wherever it applies, leaves of any kind.
- `off`.

Why `numeral`. On the step jobs below the pair counts are the same under
`numeral` and `all` (436 / 436 / 161 / 146); the win came from one or two
concats whose leaves are numerals (`numeral` hoists 2 / 1 / 0 / 0, `all`
92 / 59 / 47 / 20), and `off` leaves 608 pairs on step-vs-frag and
step-vs-cancel. On the large firedancer jobs `all` changes no pair count
and costs memory: on h2fc-rx-data-reach1 it hoists 4010 concats with
non-numeral leaves, the indices that hold them reach the decider as ites
of many leaves (328646 pairs hit its leaf budget, none without), the
re-simplification of the rebuilt query takes 30 s more, and sea-vc's peak
RSS before the solver is 11.5 GB against 9.7 GB (`numeral`) and 5.7 GB
without the fold. On h2fc-blowup-rx-end the lowering alone grew by 8 GB
(19.0 GB peak against 12.4 GB with `numeral`), past the 16 GB address
space of the benchmark runs: sea-vc stopped with an out-of-memory error
in the Ackermann lowering. Without the memo the hoist walked the ite
DAG as a tree: 2578556 hoists counted on reach1 for 4010 distinct concats.
Open: whether a non-numeral hoist pays off elsewhere (`all` keeps it
selectable).

**2. The elimination** (after the Ackermann lowering, before `--vc-defs`;
`TopConstEliminator`). A conjunct that defines uninterpreted constants as
constant values (`x = c`, `x`, `not x`, `concat(x, y, ..) = c` with
constants and numerals as parts) is removed, x := c is appended to the
model trail, and every other occurrence of x becomes c; rounds until no
conjunct is such a definition. The constants it removes are the
Ackermann reads r_k of the facts' reads (`cells`), the placement's object
bases, the exit guard, nondets (`defs`). A conjunct that defines x as two
values is not removed (substituted, it is false).

### Pointer observations

The rule: a substitution T -> c must not remove an object from the terms
that contain T; SeaM's premise rules read the objects of a pointer term
(its view, else the bases it mentions), and a base may leave a term only
by same-base cancellation or with an arm a decided condition drops
(PtrSegment.md, "The invariant"). Structurally
(`TopEqFolder::mentionsObject`): T is skipped when an object, a
`PTR_SEGMENT` term, is reachable from T without passing a memory read or
a memory (`select`, `wselect`, `store`, `wstore`), as
`seam::Rules::mentionsBase` reads a term. A read's value is memory
contents, not an address: a fact on a read at a pointer index
(`select(m, p) = c`, the facts of the step jobs) replaces the read, and
its index stays. The same test applies to each part of a concat fact; a
concat with such a part is not a fact itself, its other parts are, and
its conjunct keeps them (`concat(p, a) = c` keeps p, other facts are still substituted inside
it, p is replaced in the other conjuncts; nested concats the same;
`test/VC/fold-top-eqs/concat-object-part.mlir`). Skips
are counted `skipped-base` (terms and parts, summed over the rounds).
A T that mentions no object is substituted, pointer observations
included: the address of a pointer register (`addr(p) = c`, p a block
argument or a nondet pointer), `to_int` of one, a difference of two. No
term loses an object by it: `mentionsBase` does not look into reads, so
an offset that contains a substituted read mentions no object before or
after. Covered (`sea-fold-top-eqs-test`, ptr-facts): skipped, an
object's address, an extract or a sum of one, a difference with one, the
address of a node over one, a concat part that is one; substituted, the
same over a register, a read at an object's address. The testing switch
`--x-fold-top-eqs-no-base-skip` turns the skip off (warning
`vc.fold-top-eqs-no-base-skip`).

Without `--seam` there is no `PTR_SEGMENT`, so nothing is skipped for
mentioning an object: under `--simplify --vc-fold-top-eqs` the facts over
`PTR_FROM_INT` terms and pointer registers that the first version of the
rule skipped are substituted (exact; the fold is opt-in there).

The facts the first version of this rule skipped (every term that reached
a pointer operator) were mostly the placement's layout facts
`addr(obj.k) = c`, conjuncts of every placed query then. Substituted,
they changed what SeaM's object rules read: an address that mentions two
objects became a pointer into one, with a base no access gave it. The
case that showed it (`test/VC/fold-top-eqs/layout-facts.mlir`, from the
wide fuzz programs, `utils/fuzz-seam.py --seed 5 --accesses=wide`,
program 2): a memmove read equation reads its source at
`obj.5 + (addr(q) - addr(obj.4))`, q = `ite(c, obj.4 + 4, obj.5)`. SeaM
has no view of that term (two bases) and leaves its alias questions with
the store-map's bytes in `obj.3` open. With the layout facts substituted
and the ite pushed, one leaf was `obj.5 + 0xffffffc0`, a pointer into
`obj.5` 64 bytes before it, and `cmp.diff-base` decided its alias
question under A-SEP in the fold's round, outside any access: a premise
`--seam-check` cannot state (unexpressible, 12 firings in
`test/VC/seam/fuzz-check.test`), about a term that is no access and whose
A-INB is false wherever its leaf is selected: the decision rested on a
base the program does not give the term. An object is now the term
`PTR_SEGMENT(k, a)` (PtrSegment.md): no layout fact exists before the
boundary, which states `obj.k = c` over the erased objects, and step 2
eliminates the bases after the Ackermann lowering, as before. On the step
jobs below the Ackermann constraints were unchanged by the first rule
(436 / 436 / 161 / 146, `--vcgen-only`); there the layout facts had no
other use in the query.

## Exact

A top-level conjunct holds in every model of the query.

Step 1: Q = (T = c) /\ R. Every model of Q has T = c, so it satisfies
R[T := c], and Q' = (T = c) /\ R[T := c] has the same models as Q. This
needs `T = c` itself in Q': every key is substituted in the conjuncts that
do not own it, never on the path from its own equation to it. A split
part follows from its concat's fact (concat is injective in its parts), a
Boolean fact b gives b = true. The simplifier and the concat hoist are
equivalences. Identity is syntactic: T and its occurrences are one
hash-consed node, so substituting the node is substituting the term. No
premise, no entry of the assumption ledger, no `assume.*` line. Over the
rounds this is the invariant of the next section.

### Step 1's trail

The trail of step 1 is the map of facts, each key with the value of its
first definition, as registered: it is never rewritten. With Q_0 the
query before the fold and Q_n after the last round:

    Q_0  <=>  trail /\ Q_n

Each round, under the trail, the substitution is an equivalence, the
simplifier and the hoist are equivalences, and the facts a round adds
follow from its query. The trail itself is not sent to the solver: extra
atoms can trigger solver heuristics, and normally Q_n implies it.

Q_n alone can be weaker. Ownership is decided each round from the forms
`factsOf` reads (`T = c` either side, the parts of a concat equation, `b`,
`!b`); `keep()` protects only this round's owners. If the simplifier
turned a defining conjunct `T = c` into another form that still holds T,
the next round would find no owner and replace T inside that form too, and
`T = c` would be in no conjunct. Example (an injected rewrite, i8): [x*x =
5] is unsat; the first round maps x*x := 5; a rewrite `T = c ~> T <= c /\
T >= c` gives [x*x <= 5, x*x >= 5]; the second round gives [5 <= 5, 5 >=
5], sat.

So the result is Q_n /\ L, L the facts *lost*: replaced in a kept round in
which no conjunct owned them, and stated by no conjunct of Q_n
(`re-added`). Q_n /\ L <=> Q_0: in a round, a key replaced where it has an
owner is stated by that owner (its own keys replaced inside it are keys of
the round, again owned or lost: by induction on the term). An owner of a
concat part whose path concat holds another value in the map does not keep
the part: the conjunct does not own that concat, a key, so it is replaced
(owned elsewhere or lost), the part with it, and the conjunct becomes an
equation of two different numerals, false; the implication holds. A key
replaced without an owner is in L, and a key replaced nowhere changes
nothing; so the round's result with L implies every fact it replaced and
is equivalent to the round's input with L. The last round, whose result is
the query it started from, is dropped with its lost keys: the invariant is
over the kept rounds, and Q_n is the last kept query. A fact a conjunct of
Q_n states follows from Q_n and is not added. A fact of a term that only
left the query (its defining conjunct rewritten through another fact, `x *
z = 7` becoming `3 * z = 7` under x = 3) is not lost, unless later
rewriting makes the node again; then it is replaced without an owner and
re-added.

A lost fact whose term is a constant (`x = c`, `b`, `!b`;
`re-added-const`) is added like the others; step 2 removes it into the
model trail, so no solver sees it and a model extends by it
(`TrailModel`). A lost fact over a non-constant term reaches the solver:
dropping it is what makes Q_n weaker.

Dropping a constant's fact and extending the model by x := c is exact only
if x occurs nowhere in what is sent: not in Q_n, not in another re-added
fact. Example (injected rewrites, i8): [x*x = 9, x <= 5, x >= 5] is unsat;
the first round maps x*x := 9 and the rewrites give [x*x <= 9, x*x >= 9, x
= 5]; the second round maps x := 5 and a rewrite gives back [x <= 5, x >=
5]; the third round replaces x. Both facts are lost; x*x = 9 alone is sat
(x = 3). Hence the constant's fact is not dropped in step 1 but re-added,
and step 2 drops it the way it drops every definition: it substitutes c
for x in every other conjunct, the re-added facts included (5*5 = 9,
false), and it removes x = c only if no other definition gives x another
value (first wins in step 1 too: the map holds one value per key). This
also covers a constant that still occurs in Q_n (possible at the round
bound, where the last substitution was not simplified to a fixpoint).

Which rewrite loses an owner is not known (a hypothesis that none of the
present rules does, from reading them): the query is in normal form before
the fold, the rewriter is deterministic, and it is assumed idempotent on
normal-form conjuncts (a normal form rewritten again is itself: the
rewriter is depth-bounded per rule, RW_n, and follows its cache, so this
is not guaranteed); so a defining conjunct changes in a round only through
a substitution inside T or the hoist, and either makes a new node of T,
which leaves the conjunct; the rules at the nodes above T in its conjunct
(`=`, a concat for a split part, the top `and`) either return a fact on T,
or a term without T (`bv.eq.*`, SeaM's `eq`: decided, ite push,
cancellation; `bv.concat.*`: flatten and numeral merge keep T a part, the
merge of extracts removes it). Rules that apply at T or below it (`ac`,
`extract`, `ext`, SeaM's read rules) replace T by a new node. The counter
shows a rule that does: 0 on the 228 vcc jobs (`--lower --widen-memory=8
--seam`, numeral and all hoists). `sea-fold-top-eqs-test` (trail) injects
the rewrite above.

Step 2: Q = (x = c) /\ R with x a constant, and Q' = R[x := c]. A model M of
Q is a model of Q' (x = c in M). A model M' of Q' extended by x := c is a
model of Q: R[x := c] holds in M', x does not occur in it, and x = c holds.
Unsat answers carry over directly, and every sat model extends.

## Model trail

The trail is the ordered list of step 2's eliminations, each x := c. After
the solver answers sat, `TrailModel` wraps its model: every evaluation
substitutes the trail first (with constant values the order does not
matter; definitions by terms, not built yet, would be evaluated in reverse
elimination order). Then, as without the fold, `ReadCellsModel` and
`AckermannModel` wrap it, so the Ackermann model takes the value of an
eliminated read r_k from the trail, and its array function contains the
cell value(i_k) -> c:

    F_A = { value(i_k) -> value(r_k) }   (Ackermann.md, Model reconstruction)

The eliminated reads stay in the Ackermann lowering: their pairs with the
other reads are constrained (`(i_k = i_l) -> (c = r_l)` after the
substitution), so a model of the lowered query is a function on each
array, and `inconsistent=` stays 0 (its check is kept). Removing those
reads from the lowering instead and adding their cells to F_A after the
solve would leave the pairs unconstrained: a model could put another read
at the same index with another value, a model of the query without the
fact but not with it (an inconsistent model, a lost answer).

Why not keep the defining equations: a kept `r_k = c` is top-level work
for the solver's variable elimination, and a fact whose read the fold
removed from every other conjunct must keep a read of its cell in the
query, or the cell is missing from F_A, reads 0 in the model, and the
replay diverges. `test/VC/fold-top-eqs/post-store.mlir` @trail: the only
read of the assumed cell after the fold is its fact's; with the trail the
replay fails the assertion and validates, without it (the testing switch
`--x-model-trail-plant-skip`) the assume blocks the replay (status 2).

### On disk: `--model-trail-out=<file>`

With the fold on, the trail is written to the file (not
stdout; with `--split`, `<file>.part<N>` per part), JSON lines, one entry per elimination, in order:

    {"kind":"def","var":"obj.global.ws","value":"#x0000000008048000"}
    {"kind":"cell","var":"ack.nd.2_0.6","array":"nd.2_0",
     "index":"(bvadd #x0000000008048000 #x0000000000000028)",
     "value":"#x00000002"}

- `kind`: `cell` (an Ackermann read of an array) or `def` (another
  constant);
- `var`: the eliminated constant's SMT-LIB name;
- `value`: SMT-LIB (a numeral `#x..` / `#b..`, `true`, `false`);
- `cell` only: `array`, the lowered array's name, and `index`, the read's
  index as an SMT-LIB term with the trail substituted, so it is over the
  symbols the `--smt2` query declares (eliminated constants are not
  declared there).

A future definition entry (x := e) would give e as an SMT-LIB term over the
query's symbols and the variables of earlier entries. Replay from a file
is not built: it also needs the Ackermann read map and the name map
(`test/VC/fold-top-eqs/trail-out.test` reads the file back and checks it
against the counters and the `--smt2` query).

## Default

The maintainer's decision: the fold is on by default. A default must not
turn a valid command line into an error, so it applies where the fold
does; elsewhere it is off and says why. `vc.fold-top-eqs-status` (every
run, stdout; stderr with `--smt2`):

| status | when |
|---|---|
| `on` | the default where it applies, or `--vc-fold-top-eqs` |
| `off-flag` | `--vc-fold-top-eqs=false` |
| `off-reach` | `--reach` |
| `off-ack-relax` | `--ack-relax` |
| `off-no-simplify` | no simplifier (no `--seam`, no `--simplify`; `--seam-simplify=0`; `--mem=lambda`) |

An explicit `--vc-fold-top-eqs` (=true) keeps the errors of the excluded
modes (section Where it runs). `--model-trail-out` and
`--vc-fold-concat-ite` need the fold on, by default or by the flag; off,
they are errors that name the status.

A written query (`--smt2`, `--split-smt2`) is folded by default like a
solving run: a dumped query is the one the solver gets, so it can be
examined and run with external tools or other z3 versions. Step 2 removes
constants from it, so it is equisatisfiable with the VC, not equivalent to
it; the model trail (`--model-trail-out`) is what extends a model of the
query to a model of the VC.

The query comparisons read `--smt2` queries as the VC, so they turn the
fold off with `--vc-fold-top-eqs=false`: `utils/query-diff.py` (the gate
of a change that must not change queries), `encoding-diff.py`,
`cover-diff.py` level 1, `memops-equiv.py`. With the fold, two queries
that differ only in an eliminated fact are identical: in
`test/Tools/cover-diff-layout.test` a program input that only one side
assumes (`x == 5`) became a pass instead of a DIFF. They pass the flag
only to a binary that knows it (probed once from `--help-hidden`,
`utils/seavc_flags.py`): an older binary does not fold. A probe that
fails (the binary does not start, exits non-zero, times out) is an error
of the tool before any job: it cannot tell whether the binary folds
(`test/Tools/seavc-flags.test`). Consequence:
query-diff does not see the fold's own change to the query; the
benchmarks (default against `no-fold-top-eqs`) measure it.

`--check-inb` runs each part as a sea-vc process of its own with the
options passed on: the parts get the default.

## Where it runs, and what it does not do

- `--reach`: refused with the flag, off by default. Its targets are block
  guards, checked as assumptions outside the query; step 2 could
  eliminate one.
- `--ack-relax`: refused with the flag, off by default. Its rounds rebuild
  the lowered query from the body and the constraints.
- Needs the simplifier (`--simplify`, on under `--seam`): refused with the
  flag, off by default.
- Without the Ackermann lowering (Bitwuzla, a stored array, `--ackermannize=off`)
  step 2 eliminates constants only; a read's fact stays as a conjunct.
- Reads the boundary makes (a memory operation's read equations) exist
  only after step 1; their facts, if any, are not substituted.
- Masked facts are not used. Substituting the masked term itself would be
  exact; it is not done, to keep the measured set (an open choice).

## Counters

`vc.fold-top-eqs: facts= term= split= bool= uses= rounds= concat-ite=
concat-ite-refused= skipped-masked= skipped-untyped= skipped-base= re-added=
re-added-const=` and `vc.model-trail: cells= defs= rounds=`
(stdout, stderr with `--smt2`):

- `facts`: distinct keys registered (facts found) (term + split + bool);
- `uses`: (parent node, operand) pairs replaced, summed over the rounds;
- `rounds`: rounds of step 1 that changed the query;
- `concat-ite`: distinct concats made ites, inner ones included;
  `concat-ite-refused`: top concats the hoist applies to and
  `--vc-fold-concat-ite=numeral` refused;
- `re-added`: lost facts added to the result after the last round (section
  Step 1's trail); normally 0, a rule that rewrites a defining conjunct
  into a non-fact form otherwise;
- `re-added-const`: of those, facts of a constant (`x = c`, `b`, `!b`);
  step 2 moves them to the model trail;
- `cells` / `defs`: trail entries, Ackermann reads / other constants;
  `rounds`: rounds of step 2 that eliminated a constant.

## Measurements

Firedancer step jobs, `--lower --seam --widen-memory=4`, Ackermann
constraints (`vc.ackermann-constraints`), z3 master, seed 0:

| job | before | after | trail cells / defs | z3 after |
|---|---|---|---|---|
| step-vs-frag | 3740 | 436 | 13 / 6 | unsat, 87 s |
| step-vs-cancel | 2388 | 436 | 13 / 5 | unsat, 55 s |
| step-claims-frag | 1381 | 161 | 13 / 6 | unsat, 37 s |
| step-claims-cancel | 1366 | 146 | 13 / 5 | unsat, 4 s |

On step-claims-cancel, z3's word-level `solve-eqs` eliminates 18
variables before and none after.

Of the 436 pairs of step-vs-frag (`--ack-dump-pairs`), 290 have a read
with an ite in its index: the stored values of conditional stores, used
as array indices, under `bvzext` and `bvmul`. A hypothesis, not checked
pair by pair: the decider does not reach the leaves of an ite under those
operators, and a decider that lifted ites through them would settle more
pairs. That is the Ackermann lowering's work, not this fold's.

## Memory

The fold rebuilds every ancestor of a substituted read, which on the large
jobs is most of the query, and simplifies the rebuilt query again; the
query before the fold stays alive (the side conditions and guarantees
hold it). On h2fc-rx-data-reach1 (`numeral`) this is +3.0 GB of RSS
during the fold (substitution 1.25 GB, re-simplification 1.6 GB), and the
peak before the solver is 9.7 GB against 5.7 GB without the fold; on
h2fc-blowup-rx-end 12.4 GB against 9.5 GB. Under a fixed address-space
limit this is room the solver no longer has. Clearing the rewriter's cache
around the fold saved about 0.6 GB of the peak on reach1 (measured, not
built in).

## Open

- The default (on): measured on the jobs above only; the A/B is
  `no-fold-top-eqs` against the default.
- A premise rule firing in a round of the fold (trail context
  `fold-top-eqs`, outside any access) has no premise `--seam-check` can
  state: it is counted `unexpressible=`, never valid. With the pointer
  observations not substituted none was seen (`utils/fuzz-seam.py
  --seam-check`, modes wide / cells / bases, seeds 1, 2, 3, 5, 7, three
  programs each). If a value fact makes one appear, its alias question is
  over the images of recorded accesses (equal to them in every model of
  the query): recording the images with their units would let the
  premise be stated.
- `--arith-stats`: the origins do not follow a product the concat-ite
  hoist rebuilds (`unattributed=`; `test/VC/arith-stats.mlir` @bytes).
- `--vc-defs`: the fold eliminates the object bases, so the address sums
  become numerals plus offsets and few names are made.
- The fold's memory on the large jobs (section Memory).
- Definitions x = e with e not a numeral (solve-eqs proper): the trail
  takes them, evaluated in reverse order.
- The masked facts.
