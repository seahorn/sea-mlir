# Shared and named address sums (`sea-vc --vc-defs`)

Status: implemented, off by default (`--vc-defs=inline`). The default is
not tuned: the forms are measured on two jobs only (below), and which one
helps depends on the solver.

## The problem

The solver boundary erases every address to one flat n-ary bit-vector sum
(SeaM's normalized sum, [PtrSort.md](PtrSort.md) "The boundary"). A parser
cursor is a straight chain of adds, `o_k = o_{k-1} + d_k`, and a byte read
at `p + o_k` in each step. Its addresses are then the sums
`p + d_1 + ... + d_k + c_k`: they share their prefixes, and nothing in the
query says so.

The Ackermann lowering ([Ackermann.md](Ackermann.md)) compares the indices
of the reads pairwise. A solver comparing two flat sums cancels the common
summands and keeps the residual sum `d_{i+1} + ... + d_j` of each pair: an
adder chain per pair, not per chain step. On a firedancer parser job, sums
of up to 34 summands and about 8.1k pairs gave about 20k 64-bit adders and a
bit-blast of 7.3M expressions.

Two layers address it. The emission form (`nest`) writes each shared
sum once, as its parent's sum plus the rest, so the query is a DAG of
prefix sums. That is solver-neutral, but z3's simplifier flattens nested
sums again (`flat`, `som`) and gives the flat sums back. The z3 layer
(`hidden`) gives the same shared sums names. Naming fixes the cliff only
if the names survive the solver. z3's
`solve-eqs` eliminates a constant defined by a top-level equation
(`(= x t)`, also `bvadd` with a numeral) and substitutes the flat sum back.
Its occurrence limit (`solve_eqs.max_occs`) has no effect in current z3
versions, and the VC should not depend on a solver's preprocessing anyway.

## The step

An encoding step of the solver-facing query, after the boundary and after
the Ackermann lowering (`include/sea/Expr/SumNames.hh`). Ackermann decides
its pairs on the flat sums (same symbolic part, residues, leaves), so it
loses nothing. SeaM's rewriting runs before the boundary and never sees a
name.

- A sum is an n-ary `bvadd` of the query with at least two summands that
  are not numerals, taken as a unit `(w, M, c)`: `M` the multiset of those
  summands, `c` the numeral part modulo `2^w`. Two nodes with one unit are
  one sum.
- An index sum is a sum at an array index position (a select or a store of
  the query, an Ackermann read), through ite leaves.
- The parent of a sum `(M, c)`: the first collected sum with the same `M`
  (the multiset's representative), if this one is not that one; else the
  representative of the largest multiset among the query's sums that is a
  proper sub-multiset of `M` (ties: the first collected). Any sum of the
  query can be a parent; the cursor's offset sums (`d_1 + ... + d_k`,
  without the base) are parents of its addresses this way.
- The parents form a forest (`SumNamer::findParents`). A unit other than
  its multiset's representative has the representative as its parent, and the
  representative's own parent has a strictly smaller multiset. So a parent
  step either goes to the representative, which never has a parent of its
  own multiset, or makes the multiset strictly smaller.
- This rule is load-bearing. A relation that allowed two units of one
  multiset to be each other's parents would be unsound. Name `x + y` as
  `def.0` and `x + y + 1` as `def.1`, each defined through the other:
  `def.0 = def.1 - 1` and `def.1 = def.0 + 1`. The definitions no longer
  fix the names: at `x = y = 0` the query `x + y = 1` is false, but its
  named form `def.0 = 1` holds with `def.0 = 1`, `def.1 = 2`.
- Named: every index sum and every ancestor of one, except an index sum with
  no parent and no child. Other sums are not touched: the step names the
  shared offset arithmetic, not every sum.
- A name `def.<k>` (`w` bits; not with `nest`) is defined as
  `def.P + (M - M_P) + (c - c_P)`
  for a parent `P`, else as the sum itself. Every named sum is replaced by
  its name, everywhere in the query and in the definitions.

So the chain becomes `def.k = def.{k-1} + d_k + c`, and an Ackermann pair
compares two names. With `nest` the same structure is emitted without
names: every named sum is replaced by its definition term, one node shared
by all its uses, and nothing is asserted.

## The definition form

`--vc-defs=`:

- `inline` (default): no names; the query is unchanged.
- `nest`: no names, no definitions: the shared prefix sums as nested
  `bvadd` nodes (the emission form).
- `eq`: `(= def.k t)`. For comparison: z3 eliminates every such name.
- `hidden`: `(bvule def.k t)` and `(bvule t def.k)`. `bvule` is
  antisymmetric, so the pair is exactly the equation; it is not an
  equation, so no solver's variable elimination solves it.

## Soundness

No assumption. Each name is a fresh constant fixed by its definition: a
model of the query extends to the names uniquely, and a model of the named
query restricted to the original constants is a model of the query. The
names are not program values. Replay, model evaluation and validation read
the original terms, so they never see a name. The names `def.<k>` cannot
collide with VC names (`%`, `^`, `nd.`, `obj.`, `ack.`); sea-vc checks that
no VC constant starts with `def.` (an error otherwise), and the unique-name
check runs again after the step. The origins of `--arith-stats` follow the
query through the step (`SumNamer::image`).

The statements of this section are machine-checked in seam-lean (module
`SeaM/VCDefs`; the proofs are to be published), over a
one-width model of terms with nested sums, for every plan that meets the
invariants the step establishes (the parent rule above among them):
each definition is exact and the rewritten query is the query (A1); the
dependency of the names is well-founded (A2); the named query is
equisatisfiable with the query and the names are determined (A3); the
`hidden` atom pair is the equation, at every width (A4); the `nest` form
is the query, for any number of substitution rounds (A5). Not proved: that
finitely many rounds remove every name (the step terminates by A2).

## Counters

`vc.defs: inline|nest|eq|hidden`; with a form, `vc.defs-sums` (distinct
sums), `-index-sums`, `-names` (with `nest`: the shared sums), `-atoms`
(asserted definition atoms: 0 with `nest`, 1 per name with `eq`, 2 with
`hidden`), `-rewritten` (`bvadd` nodes replaced).

## Measured

Two firedancer jobs, `--lower --seam --widen-memory=4`, z3 5.3.3 (seeds
0-3: `sat.random_seed` = `smt.random_seed`), Yices 2.7.0 and Bitwuzla
0.9.1 (one run each, through `sea-vc --solver`; Bitwuzla without the
Ackermann lowering, its default). Wall seconds, timeout 900, on a shared
machine with other solver jobs running: the times are noisy. Bit-blast:
z3's `num-exprs` after `bit-blast` (`-v:10`); aig: z3 runs its post-blast
aig step only below 300 MB.

`txn-pf-v1-writes` (unsat, 8101 Ackermann pairs):

| form | z3 bit-blast | aig | z3 seeds 0-3 | Yices | Bitwuzla |
|---|---|---|---|---|---|
| inline | 7.34M | no | 900+, 164, 218, 497 | 105 | 21 |
| nest | 7.34M | no | - | 22 | 13 |
| eq | 7.36M | no | - | - | - |
| hidden | 2.27M | no (352 MB) | 90, 55, 90, 47 | 79 | 35 |

`txn-reach-v0-alt` (sat, 39287 Ackermann pairs):

| form | z3 bit-blast | z3 seeds 0-3 | Yices | Bitwuzla |
|---|---|---|---|---|
| inline | 10.44M | 347, 900+, 900+, 900+ | 5.0 | 14 |
| nest | 10.44M | - | 16 | 11 |
| hidden | 8.94M | 195, 157, 233, 176 | 6.2 | 40 |

- `nest` alone does nothing for z3: its simplifier flattens the nested
  sums again, and the bit-blast is the flat one exactly. `eq` names are all
  eliminated by `solve-eqs` (43 eliminated variables instead of 5).
- With `hidden`, every Ackermann pair compares two names. What is left of
  the bit-blast is mostly the pairs themselves: one 64-bit equality each
  (without them the `txn-pf-v1-writes` query bit-blasts to 0.96M). On
  `txn-reach-v0-alt`, 39k pairs keep it at 8.9M: naming cannot reduce the
  number or the width of the comparisons.
- `--narrow-offsets` ([Ackermann.md](Ackermann.md), "Narrowed offsets",
  rests on A-INB) compares two reads of one object at k bits, 2^k >= the
  object's size; a narrowed read's index sum is then its k-bit offset, and
  the names of the prefix chain are k bits wide.
- Yices and Bitwuzla share common sums themselves; names cost them up to
  2x, while `nest` was the fastest form for both on `txn-pf-v1-writes`.

Controls (z3, seed 0 unless noted):

- `eq` with `smt.solve_eqs=false`: bit-blast 2.07M, no aig, 122 s. The
  hidden form gets the same effect without a solver option.
- A replica of z3's QF_BV tactic with `flat=false` and `som=false` in every
  simplify step, plus `(set-option :rewriter.flat false)` (the tactic
  alone still gives 7.38M on `nest`):

  ```
  (check-sat-using (using-params (then
    (using-params simplify :flat_and_or false :flat false)
    (using-params propagate-values :flat_and_or false)
    (using-params solve-eqs :solve_eqs_max_occs 2)
    elim-uncnstr bv-divrem-bounds reduce-bv-size
    (using-params simplify :som false :pull_cheap_ite true
      :push_ite_bv false :local_ctx true :local_ctx_limit 10000000
      :flat false :hoist_mul false :flat_and_or false)
    (using-params simplify :hoist_mul true :som false :flat_and_or false
      :flat false)
    max-bv-sharing ackermannize_bv bit-blast
    (if (< memory 300)
        (then (using-params (then simplify solve-eqs) :local_ctx true
                :flat false :flat_and_or false) aig)
        skip)
    sat) :elim_and true :push_ite_bv true :blast_distinct true))
  ```

  | job, form | bit-blast | z3 |
  |---|---|---|
  | pf-v1-writes, inline | 3.12M | 102 |
  | pf-v1-writes, nest | 1.54M (aig runs) | 48, 81, 73, 51 (seeds 0-3) |
  | pf-v1-writes, hidden | 2.20M | 41 |
  | reach-v0-alt, nest | 4.81M | 898, 900+, 900+, 900+ (seeds 0-3) |

  The no-flatten tactic gives the smallest bit-blast on both jobs and
  makes the names unnecessary on the unsat job, but on the sat job it was
  much slower than `hidden` with z3's default tactic. One sat job does not
  settle it; whether sea-vc should pass its own tactic is open.
