# Ackermann lowering of read-only arrays

`include/sea/Expr/Ackermann.hh` (`Ackermannizer`, `AckermannModel`);
`sea-vc --ackermannize=auto|on|off`; tests `unittests/Ackermann`
(`test/Expr/ackermann.test`), `test/VC/ackermann/`.

## Why

After SeaM's memory-operation terms (SeaM-memops.md) the query of a
`--seam` run often has no store: only reads `select(A, i)` of array
constants (initial memories `nd.<site>`, memhavoc arrays). Such an A is an
uninterpreted function of its index. z3 lowers these reads itself
(`ackermannize_bv` tactic) only while the number of Ackermann lemmas stays
at or below `rewriter.div0_ackermann_limit` (default 1000; z3
`src/ackermannization/ackermannize_bv_tactic.cpp:47,67`): 32 reads (496
lemmas) go to the SAT engine, 64 reads (2016) fall back to the SMT engine
(`-v:2`: `(smt.searching)` vs `(sat.*)`). hash_iter_begin (281 distinct
reads) and array_list_swap (98) ran in the SMT engine. We do not rely on a
solver heuristic for something this simple with such bad consequences: the
lowering is ours, the same for every solver.

## Where

An encoding step of the solver-facing query, after the solver boundary
(`SeaOpSem::solverTerm`) and the unique-name check, before the query goes
to the solver (and `--smt2`, `--dag-stats`, which show what the solver
gets). It is not a program change (no pass) and not a SeaM rewrite: it
changes neither the meaning of SeaM terms nor the `--seam-check`
obligations (which are stated on the unlowered terms).

`--ackermannize=auto` (default): on when the query has no store node,
except with `--solver=bitwuzla` (slower with the lowering on every measured
query, below).
`on`: always; an array that is used otherwise is kept (below), so a query
with stores is lowered partially. `off`: never.

## What is lowered

An array constant A (a 0-ary application of sort `(Array (_ BitVec m)
(_ BitVec n))`) is lowered iff every occurrence of A in the query is the
array operand of a `select`: no store over it, no equality of arrays, no
ite over arrays, no other operator. Otherwise A is kept as it is
(`vc.ackermann-kept`). If the query has a quantifier or lambda, nothing is
lowered (a read could depend on a bound variable; VC queries have none).

Each distinct read `select(A, i_k)` becomes a fresh constant
`r_{A,k}` = `ack.<A>.<k>` of the cell sort. The reads are replaced
bottom-up, so an index that itself reads an array (a loaded pointer) is
lowered first: the index term i_k recorded in the table is in terms of the
fresh constants.

## The constraint set

For each lowered array A, for each pair k < l of its distinct reads that
is not dropped below:

    i_k = i_l  ->  r_{A,k} = r_{A,l}

(conjoined with the query). The lowered formula is equisatisfiable with the
original one: a model of the original gives values to the r's by reading
A; a model of the lowered one defines A as a function (below) because of
the constraints.

Pair reductions. Each index is put in a linear form `S + c` over w-bit
bit-vectors (`ackermann_detail::LinearForms`): `bvadd`, `bvsub`, `bvneg`,
`bvmul` with one non-numeral factor are expanded, numerals summed modulo
2^w, and `ite(b, S1 + c, S2 + c)` becomes the atom `ite(b, S1, S2)` plus
c (hash-consed, so equal structures are equal atoms; `ite(b, S + c, S + c)`
is `S + c`); every other term is an atom; S maps atoms to nonzero
coefficients modulo 2^w. Each step is an identity of w-bit arithmetic, so
the form equals the index for every assignment. Then:

- same S and same c: the indices are equal; the reads share one constant
  (one read). This covers the same index term (one hash-consed node), two
  numerals equal modulo 2^w, `x + 1` vs `1 + x`, and SeaM's memcopy source
  index `s + ((d + k) - d)` vs `s + k`.
- same S, different c: `S + c1 = S + c2` iff `c1 = c2 (mod 2^w)`, so the
  constraint is valid (its premise is false): not added. Counted as
  `numeral-pairs` (S empty: two numerals) and `offset-pairs` (S not empty).

- different S, offsets that differ below the common power of two of the
  difference: `S_k + c_k = S_l + c_l` needs `S_k - S_l = c_l - c_k (mod
  2^w)`; if every coefficient of `S_k - S_l` (atoms of both, coefficients
  subtracted: a shared base such as obj.k cancels) is a multiple of 2^t and
  `c_l - c_k` is not, the indices are never equal: not added. Counted as
  `residue-pairs`. This is the field of one slot against another field of
  any slot of an array of records: `T + 24 h_1 + 2` vs `T + 24 h_2 + 3`
  (t = 3, difference 1). Identity of w-bit arithmetic, no assumption.

- leaf pairs: the two indices taken leaf by leaf (through ite, and bvadd /
  bvsub over ites: the linear form of every combination, at most 16 per
  index), with the layout values substituted for the object constants
  (obj.k = <numeral>: unconditional side conditions of the query, so true
  in every model; `Ackermannizer::setValues`, used for this test only, not
  for read sharing); never equal if no leaf of one can equal a leaf of the
  other (different numerals, equal symbolic parts with different numerals,
  a residue difference): not added. Counted as `leaf-pairs`. Found by the
  equality audit (sea-vc --eq-audit): on vcc 22% of the sampled
  constraint pairs (42 jobs) were decided by the layout alone, e.g.
  `obj.stack.13 + 55` vs `obj.stack.21 + 50`, or an ite of such pointers.
  string_eq_ignore_case 1169 -> 617 constraints, linked_list_swap_contents
  1015 -> 108.

One more drop, on by default (`--ack-stride=false` turns it off): the stride rule (--ack-stride, docs/design/PtrIndex.md section 5): a pair
of reads whose every address was a promised GEP node (inbounds / nusw /
nuw) over one base, whose integer offsets can never be equal (the gcd of
the stride differences does not divide the constant difference), or one at
a node over an object of size 0. Dropping any constraint only weakens the
lowered query: unsat stays sound with no premise. The promises (and, for a
byte of a wide access or a size-0 base, A-INB) are what make the dropped
constraints vacuous, so a sat model needs them: one that puts two such
reads at one index breaks one, is detected (AckermannModel::strideBroken)
before the result, and sea-vc reruns without the rule.
Counted `vc.ackermann-stride-pairs` (of them `-stride-a-inb-pairs`) and
why the rest were not (`-stride-not-eligible`, `-other-base`, `-divides`).
On vcc hash_table_eq (seam-wide, yices): 95104 -> 43712 constraints, solve
54 -> 26 s.

No other pair is dropped. In particular pointers into different objects
(`obj.a + 8` vs `obj.b + 8`) keep their constraint (a possible extension,
with the same argument: a drop never makes unsat unsound).
(Possible extension, not done: drop pairs SeaM decided different, with the
premise stated and reported as for the rewriter.)

Reported (`vc.ackermann-*`, with `--brunch-stats` as BRUNCH_STAT):
arrays lowered / kept, distinct reads, pairs = sum of n(n-1)/2, numeral,
offset, residue and leaf pairs, constraints added (= pairs - numeral -
offset - residue - leaf pairs).

The point of the lowering is that no solver has to Ackermannize (z3 does it
only below its lemma limit, else falls back to its SMT engine); its size is
the number of index equalities nothing decided, so SeaM's provenance
tracking and these reductions keep it small. hash_table_eq (vcc,
`--widen-memory=8 --seam`): 1442 reads,
781264 pairs; 750752 constraints (query 3.0M nodes, Yices T/O, z3 unknown)
before; two fixes: the pointer view of a null leaf whose offset is a large
DAG (SeaMRules `mentionsBase` was a 64-node tree walk answering "mentions a
base" when exhausted; now exact and memoized), which let addr.prune-size0
drop 704 reads' null leaves (no gain in pairs by itself: the reads are in
one object), and residue pairs: 655648 dropped, 95104 constraints (415k
nodes); Yices 120 s, z3 75 s wall (Ackermann off: 38 / 162 s). The 95k
left are reads of different hash slots with the same field offset modulo 8
(deciding them needs ranges of the masked hashes).

Measured: hash_iter_begin (`--lower --widen-memory=8 --seam`) 281 reads,
39340 pairs, 36884 offset pairs, 2456 constraints; array_list_swap 98
reads (130 before sharing equal linear forms), 4753 constraints (its
indices differ in their symbolic parts).

## Measurements (Release, timeout 300 s)

`--lower --widen-memory=8 --seam`, sea-vc solve-time, off -> on:

| query | z3 | yices | bitwuzla |
|---|---|---|---|
| hash_iter_begin (2456 constraints) | 2.13 -> 2.09 | 1.37 -> 0.21 | 4.95 -> 12.70 |
| array_list_swap (4753 constraints) | timeout -> timeout | 11.5 -> 72.7 | 8.1 -> 50.0 |

z3 binary on the `--smt2` dumps (`-v:2`, engine; wall):

| dump | default div0_ackermann_limit | limit 10^8 |
|---|---|---|
| hash_iter_begin off | smt, 2.76 | sat, 2.23 |
| hash_iter_begin on | sat, 2.42 | sat, 2.35 |
| array_list_swap off | smt, 216 | sat, timeout |
| array_list_swap on | sat, 272 | sat, 278 |

The lowering puts z3 in the SAT engine, as intended, but that alone does
not make it faster: on array_list_swap the SMT engine (lazy array lemmas) is
the faster one, and the eager constraints (64-bit index equalities,
bit-blasted: ~540k SAT variables) are the cost. vcc-small (bench.py,
configs vcc-seam-ack-{on,off}-{yices,z3}): same verdicts, totals within
noise except array_eq with z3 (0.48 -> 1.24 s). Yices gains where the
constraints are few relative to the reads (hash_iter_begin: 36884 of 39340
pairs dropped as offset pairs) and loses where they are many.

## Model reconstruction

A model of the lowered query has values for every i_k and r_k, none for A
(the solver never saw A). `AckermannModel` (a `solver::Model` wrapping
the solver's) rebuilds A as the finite function

    F_A = { value(i_k) -> value(r_k) }   with default 0

consistent by the constraints (two reads with equal index values have
equal cell values; a pair that is not, `inconsistent`, is a lowering bug:
sea-vc reports error `vc.ackermann-inconsistent`, status 2). Its `eval(e)`
rewrites e bottom-up: `select(A, j)` becomes `F_A(value(j))` (j rewritten
and evaluated first), any other occurrence of A becomes the store chain of
F_A over a constant-0 array, then the inner model evaluates the result.
sea-vc wraps the model once and hands the wrapper to replay
(`ModelSource::get` / `getCell`) and validation, so every consumer reads
the same arrays.

Default 0: a read at an index the query does not read (`defaulted`,
reported in `vc.ackermann-model: defaulted=<n> inconsistent=<n>`) is a cell
on which the query does not depend (for example a load whose value nothing
uses, or a cell a memmove copies that is never read again), so any value
satisfies the query; 0 is the choice. Replay and validation use the same
value, so it causes no mismatch; if the interpreter and the VC disagree on
an address, validation sees the address values differ.

## Not handled

- Arrays used other than under select (kept: the solver's array theory).
  `--ackermannize=auto` lowers only queries with no store, so the plain
  (non-SeaM) VC and widened memories with store chains are left to the
  solver.
- Pair reductions beyond linear forms (no ARM, no A-INB, no case split on
  ite conditions).
- The number of constraints is quadratic in the reads of one array with
  different symbolic parts (array_list_swap: 4753), and it is what makes
  the lowered query slower there for every solver. Fewer constraints need
  more pairs decided different: by ARM / A-INB (stated premise, as for the
  rewriter), or by a different reduction (z3-style lazy lemmas, a
  sorting-based encoding). Not implemented.
