# Folding the top-level facts of the VC

`include/sea/VC/FoldTopEqs.h`, `lib/VC/FoldTopEqs.cpp`; `sea-vc
--vc-fold-top-eqs`, off by default. Tests: `test/VC/fold-top-eqs/`.

Status: implemented, off by default (exact; the default is open).

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
it fixes the bits of x under m only.

Every occurrence of a fact's T in the other conjuncts becomes c. The
defining conjunct stays, rebuilt with its own T kept (`T = c` never
becomes `c = c`); the other facts are substituted inside it too. Then the
whole query is simplified again (the same rewriter, so ites, read guards
and comparisons fold), concats whose parts are numerals and ites on one
condition become ites of concats (`concat-ite`; the Ackermann decider
takes the leaves of ites, not of concats), and the round repeats until the
query does not change, collecting new facts each round (`rounds`). A bound
of 64 rounds stops it with the warning `vc.fold-top-eqs-rounds` (exact
either way).

**2. The elimination** (after the Ackermann lowering, before `--vc-defs`;
`TopConstEliminator`). A conjunct that defines uninterpreted constants as
constant values (`x = c`, `x`, `not x`, `concat(x, y, ..) = c` with
constants and numerals as parts) is removed, x := c is appended to the
model trail, and every other occurrence of x becomes c; rounds until no
conjunct is such a definition. The constants it removes are the
Ackermann reads r_k of the facts' reads (`cells`), the placement's object
bases, the exit guard, nondets (`defs`). A conjunct that defines x as two
values is not removed (substituted, it is false).

## Exact

A top-level conjunct holds in every model of the query.

Step 1: Q = (T = c) /\ R. Every model of Q has T = c, so it satisfies
R[T := c], and Q' = (T = c) /\ R[T := c] has the same models as Q. A split
part follows from its concat's fact (concat is injective in its parts), a
Boolean fact b gives b = true. The simplifier and the concat hoist are
equivalences. Identity is syntactic: T and its occurrences are one
hash-consed node, so substituting the node is substituting the term. No
premise, no entry of the assumption ledger, no `assume.*` line.

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

With `--vc-fold-top-eqs`, the trail is written to the file (not
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

## Where it runs, and what it does not do

- `--reach`: refused. Its targets are block guards, checked as
  assumptions outside the query; step 2 could eliminate one.
- `--ack-relax`: refused. Its rounds rebuild the lowered query from the
  body and the constraints.
- Needs the simplifier (`--simplify`, on under `--seam`).
- Without the Ackermann lowering (Bitwuzla, a stored array, `--ackermannize=off`)
  step 2 eliminates constants only; a read's fact stays as a conjunct.
- Reads the boundary makes (a memory operation's read equations) exist
  only after step 1; their facts, if any, are not substituted.
- Masked facts are not used. Substituting the masked term itself would be
  exact; it is not done, to keep the measured set (an open choice).

## Counters

`vc.fold-top-eqs: facts= term= split= bool= uses= rounds= concat-ite=
skipped-masked= skipped-untyped=` and `vc.model-trail: cells= defs= rounds=`
(stdout, stderr with `--smt2`):

- `facts`: distinct keys registered (facts found) (term + split + bool);
- `uses`: (parent node, operand) pairs replaced, summed over the rounds;
- `rounds`: rounds of step 1 that changed the query;
- `concat-ite`: concats made ites;
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

## Open

- The default (off): not measured beyond the jobs above.
- Definitions x = e with e not a numeral (solve-eqs proper): the trail
  takes them, evaluated in reverse order.
- The masked facts.
