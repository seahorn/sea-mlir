# Canonical linear forms of the query (`sea-vc --vc-canonical-sums`)

Status: implemented, off by default. The default is not tuned: whether the
form helps depends on the solver and on its preprocessing, and it is to be
measured per solver and pipeline.

## The problem

A bit-blasting solver turns each sum of the query into an adder chain. The
same linear form spelled in two ways gives two chains that share no gates,
and the solver has to prove them equal bit by bit. The VC spells one form
in many ways:

- an equality isolates one term, `zext x = c - y - z`, while another use
  of the same values writes `x + y + z`;
- `-1` is distributed per term (`bvsub`, `bvneg`, `bvmul -1`), so the
  negated part is not one chain;
- the constant comes first in one sum and last in another;
- the order of the summands follows the program, and differs between a sum
  and its zero-extended copy at another width.

The step spells every linear form one way, so equal forms are one
hash-consed term and similar ones share their chains.

## The form

A *linear node* is a `bvadd` (any arity), a binary `bvsub`, a unary
`bvneg`, or a `bvmul` with at most one factor that is not a numeral. Its
form is `c + k_1 * l_1 + ... + k_n * l_n` modulo `2^w`; the leaves `l_i`
are the other bit-vector nodes, with their own kids rewritten first. A
*maximal* linear term is a linear node used by a node that is not linear.

1. **Sums.** A maximal linear term is emitted as `P`, `(bvadd P c)`,
   `(bvadd (bvadd P (bvnot N)) c+1)`, `(bvnot N)`, `(bvadd (bvnot N) c+1)`
   or the numeral `c` (`c+1` folded modulo `2^w`; a zero numeral is not
   written).
   - `P` is the n-ary `bvadd` of the positive leaves, `N` the n-ary
     `bvadd` of the negative ones; a single leaf is the leaf itself.
   - A coefficient `k > 2^(w-1)` puts its leaf into `N` with `2^w - k`;
     every other coefficient `k != 1` is written `(bvmul k leaf)`. The
     coefficient `2^(w-1)` is its own negation and stays positive.
   - There is no `bvsub` and no `bvneg` in the output, and no per-term
     negation: `P - N = P + ~N + 1`.
   - A general factor is not distributed over a sum: `3 * (x + y)` is a leaf
     (its sum is canonical inside); `-1 * (x + y)` and `1 * (x + y)` are
     linear.
2. **Leaf order.** A leaf has a key `(base, eff)`: the low `eff` bits of the
   node `base`, zero-extended to the sum's width. `zext x`, `(concat 0 x)`
   and a low `(extract k-1 0 x)` name low bits of `x`, so they have the
   base `x` (through nested ones too); any other node is its own base, at
   its width. Leaves are ordered by the creation id of the base
   (`ENode::getId`, as `BvRules.hh` orders its sums), then by `eff`. So `x`,
   `zext x` and a low extract of `x` sit at one place in every sum, at
   every width. Two leaves of one key in one sum are one leaf, their
   coefficients added; a key at a width is spelled as the first node of it
   met in the walk. Equal forms give one hash-consed term.
3. **Equalities.** An equality one of whose sides is a sum (a linear node
   that is not a numeral and not one leaf with coefficient 1) is balanced.
   Its form `D = a - b`, or `-D`, is written as `(= P -c)`,
   `(= (bvadd P c) N)` or `(= P N)`; `true` or `false` if `D` has no leaf.
   Of `D` and `-D` the one with fewer negative leaves is kept, ties broken
   by the sequence of leaf keys, then by the smaller constant (when every
   coefficient is `2^(w-1)`, `D` and `-D` have the same leaves: `128y = 3`
   and `128y = 253` at 8 bits): `a = b` and `b = a` give one term. No term
   is isolated.
4. **Full width.** An extract over a sum is a leaf whose base is the sum,
   which is canonical at its own width: the extract stays on top. Pushing
   a low extract into the sum (truncating its summands) is exact too, but
   gives a second, narrower chain of the same sum, which shares nothing
   with the full one.
5. **Bitwise terms.** `bvnot`, `bvxor`, `bvand`, ... are leaves: `~x = y`
   stays as it is, and `~x + 1` stays the sum of the leaf `~x` and `1`.

Item 6 of the original list, no z3-only operator in the `--smt2` dump, is
the portable dialect of `--smt2` (`--smt2-dialect=portable`), a separate
change. The two are independent: this step makes no operator that is not
in the query already, except `bvadd`, `bvmul` by a numeral, `bvnot` and
numerals.

## Where it runs

An encoding step of the solver-facing query (`include/sea/Expr/CanonicalSums.hh`),
in `sea-vc` after the solver boundary (`SeaOpSem::solverTerm`: plain bit-vectors,
no pointer sort, no SeaM memory terms), the Ackermann lowering, the
model-trail elimination of `--vc-fold-top-eqs` and the place of
`--vc-defs`, as the last change of the query. The in-process solver and the
`--smt2` dump get the same `Expr`, so the flag changes both alike.

- SeaM's rewriting (`SeaMRules.hh`, `BvRules.hh`) keeps its own normal form
  of sums (bases first, then by id, the numeral last; `bv.eq.linear`'s
  `A + d = B`). Its rules read that form, so this step runs after it and
  is not a rule of that rewriter.
- No existing rule pushes an extract into a sum (item 4): `BvRules.hh` has
  extract-of-extract and extract-of-concat only. The one place that
  computes sums at fewer bits is the Ackermann lowering's narrowed offsets
  (`--narrow-offsets`, A-INB, `LowBits.hh`): its k-bit sums are sums at their
  own width here, and nothing is widened back.
- The Ackermann lowering decides its pairs on the query before this step,
  so its pair tests (`LinearForms.hh`) see the forms they were written for.

What sees the step's terms:

- after it: the query of the solver of the verdict, the `--smt2` dump,
  `--split-smt2` and every split part, the reach queries, the parts of
  `--check-inb` (sea-vc processes with the job's options) and the rounds
  of `--ack-relax`;
- before it: the side checks, which build their own terms from the VC
  (`--seam-check`, `--narrow-check`, `--split-check`, `--eq-audit`,
  `--words-check`), and the model trail written by `--model-trail-out`.
  Their questions are about the VC, not about its spelling.

Interactions:

- `--vc-defs` (named or nested address sums, [SumNames](SumNames.md)) is
  rejected with this flag: its nested sums are maximal linear terms here
  and would be flattened, and its names would be the leaves of other sums.
  Which of the two serves a solver better, or how to combine them, is open.
- `--ack-relax`: its parts are renamed through the step's image, as through
  `--vc-defs`; with every candidate kept, the query is the whole query.
- `--arith-stats` and the other origin users follow the query through the
  step's image: a non-linear node and a maximal sum go to their rewritten
  term; a multiplication `k * x` inside a sum goes to the term its leaf
  gets in the chain, `(bvmul k' x)`, which is in the query unless its
  coefficient merged with another term of `x` (`2x + x = 3x`: then the
  product is unattributed). A product the step makes from a sum that had
  no multiplication (a coefficient from repeated terms) has no origin
  either and counts as unattributed. A sum inside a sum is no term of the
  query and has no image.
- Replay and model evaluation read the original terms. The step keeps
  every constant that matters: a constant can only disappear when its
  coefficients cancel (`x - x`), and then no value of the query depends on
  it; the model may then not assign it, as after `--simplify`.

## Exactness

No assumption. Every step is an identity of SMT-LIB bit-vector
arithmetic, checked with z3 at widths 1 to 64 before coding:

- a form is equal to its term modulo `2^w` (`bvadd`, `bvsub`, `bvneg`,
  `bvmul` by a numeral are the ring operations);
- `k * x = -((2^w - k) * x)`;
- `P - N + c = (P + ~N) + (c + 1)` and `-N + c = ~N + (c + 1)`;
- `a = b` iff `a - b = 0`; `P + c = 0` iff `P = -c`; `P - N + c = 0` iff
  `P + c = N`;
- `zext x = concat 0 x`, `zext (zext x) = zext x`, the low bits of
  `zext x` are the low bits of `x` (or `zext x` itself), the low bits of a
  low extract are the low bits of its argument: two leaves of one key are
  one value.

The unit test `sea-canonical-sums-test` (`test/Expr/canonical-sums.test`)
checks each case on exact terms with a z3 miter, and random terms at widths
1 to 64 (sums, differences, negations, products by numerals, products of
two terms at up to 8 bits, zext / concat / low and high extracts, bitwise
terms, ite, in equalities and compares): each with a miter (Yices, else
Bitwuzla, else z3, with a limit; unknowns are shown) and at random ground
points (z3; always decided), and that two fresh factories give the same
text. When the miter is unknown, the ground check takes 64 points and
the corner values `0`, `1`, `2^(w-1)`, `2^w - 1` of each constant; the
number of unknowns is bounded in the lit test. z3 does not decide some
16-bit miters of rearranged adders within minutes; Yices and Bitwuzla decide them in milliseconds, which is the
problem this step addresses. The lit tests (`test/VC/canonical-sums/`)
check items 1 to 5 on `--smt2` and the replay of a model of the
rewritten query.

## Representation at the solvers

The step emits an n-ary `bvadd` per chain. Two backend facts decide what
the solver sees; both are older than this step:

- The z3 and Yices marshallers build an n-ary `bvadd` as a right-nested
  chain of binary adds, `a + (b + (c + d))` (`ExprToZ.def`,
  `MarshalYices.cpp`). Two chains then share their common suffixes, not
  their prefixes.
- z3's SMT-LIB printer writes nested associative applications flat
  (`pp.flat_assoc`), and its parser makes one n-ary node of the flat
  text. So the `--smt2` dump of a nested sum, read back, is not the term
  the in-process solver gets: `(bvadd (bvadd P (bvnot N)) c)` is printed
  `(bvadd p1 p2 (bvnot N) c)`. This holds for every nested sum of the
  query, with or without this step.

Whether the marshallers should build n-ary nodes (z3 accepts them) and the
dump print the nesting is a separate decision: it changes the default query.

## Counters

With the flag (with `vc-time`; stderr with `--smt2`): `vc.canonical-sums:
on`, `vc.canonical-sums-sums` (maximal sums emitted, not counting numerals
and single leaves), `-changed` (of them, a different node than the input),
`-leaves` (leaves of the emitted chains, equalities included), `-eqs`
(balanced equalities), `-eqs-changed`. A `bvsub` or `bvneg` of another
arity is a leaf, with the warning `vc.canonical-sums-opaque`.

## Cost and limits

- The form of every linear node is kept; a chain of nested adds of length
  `n` costs `O(n^2)` coefficients. The address sums of the VC have tens of
  summands.
- The step is not idempotent: run on its own output, `(bvnot N)` is a leaf
  of the outer sum, which is then one flat chain `P + ~N`. It runs once.
- The leaf order is the creation order of the bases: deterministic for one
  input and one run, as `BvRules.hh`'s order; not a property of the
  formula alone.

## Open

- The default (off) and the combination with `--vc-defs`.
- The marshallers' fold direction and the dump's flat printing (above).
- Whether a solver's own preprocessing keeps the form: z3's default
  pipeline starts with a simplifier that flattens sums (`flat`, `som`), so
  for z3 the form is expected to matter with a pipeline that does not
  (`--z3-tactic`) [hypothesis, to be measured].
