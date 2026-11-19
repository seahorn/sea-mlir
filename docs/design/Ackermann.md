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

### The difference decider

`include/sea/Expr/DiffDecider.hh` (ranges: `include/sea/Expr/BvRange.hh`;
tests `unittests/DiffDecider`, `test/Expr/diff-decider.test`); on by
default, `--ack-decider=false` off. It runs on the pairs the tests above
leave, before the stride rule, on the index the lowering compares and at
its width: the w-bit indices (a narrowed read's index is `base +
zext(low)`), and the k-bit offsets of two narrowed reads of one object. A
NEVER at 64 bits is not one at k bits: offsets 0 and 2048 differ at 64
bits and are equal at k = 11.

For two terms a, b it answers NEVER (never equal: no constraint), ALWAYS
(always equal: counted, the constraint is kept) or UNKNOWN, with the
premise used: none, or LAYOUT (the object constants replaced by their
numerals, the conjuncts `obj.k = <numeral>` of the query, so the drop is
exact for the query as given). The difference `D = a - b` is a linear form
`sum s_i x_i + c` over w bits; each atom x_i has an interval of integer
representatives (BvRanges, exact: numerals, zext, sext, concat, extract,
bvand, bvor, bvurem, bvudiv, shifts by a numeral, bvadd / bvsub / bvneg /
bvmul by a numeral without reduction, ite as the hull; everything else the
full range at its width). As an integer D lies in `[lo, hi]` and is
congruent to `c` modulo `g = gcd(s_i)`; `a = b` iff `D = 0 (mod 2^w)`.
Classes (counted by the weakest a pair needed):

- residue: `c` is not a multiple of `gcd(g, 2^w)` (also: no atom, c != 0);
- interval: no multiple of `2^w` in `[lo, hi]`;
- congruence: no multiple `m` of `2^w` in `[lo, hi]` with `m = c (mod g)`;
- signed: the two above with the signed representatives of the atoms
  (`sext(x)` read as the signed x, negative numerals) instead of their
  unsigned values: an nki tensor index `o + c sext(x) + K`, where the
  unsigned reading of the sext spans 2^w.

Leaves. The unsplit forms are tested first (an ite is an atom; an ite common
to both indices cancels), then leaf by leaf: each index as the set of
linear forms of its leaves through ite, bvadd and bvsub, each leaf with the
ite conditions it was taken under; a combination that takes one condition
(the same node) both ways is infeasible and dropped. NEVER if every
feasible leaf pair is NEVER. Splitting is not monotone, so both are tried.
Budgets (`--ack-decider-leaves`, default 256 leaves per node, else that
node is one leaf; `--ack-decider-leaf-pairs`, default 16384 leaf pairs per
read pair, else no split): a first guess (untuned). Each test runs
first without the layout values, then with them.

A wrong NEVER is not a wrong unsat: dropping a constraint only weakens the
lowered query, and a sat model that puts two dropped reads at one index
with different cells is caught by the model check below
(`vc.ackermann-inconsistent`, status 2).

Counted `vc.ackermann-decider-pairs` (dropped), by class `-residue`,
`-interval`, `-congruence`, `-signed`; of them `-layout` (premise LAYOUT)
and `-split` (decided leaf by leaf); `-always` (kept); `-budget`
(undecided pairs on which a budget was hit).

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
Counted `vc.ackermann-stride-pairs` (of them `-stride-a-null-pairs`, `-stride-a-nowrap-pairs`) and
why the rest were not (`-stride-not-eligible`, `-other-base`, `-divides`).
On vcc hash_table_eq (seam-wide, yices): 95104 -> 43712 constraints, solve
54 -> 26 s.

One more, off by default: the ownership drop (`--ack-own`, below), on the
pairs every rule above leaves. No other pair is dropped. Without
`--ack-own`, pointers into different objects (`obj.a + i` vs `obj.b + j`)
whose offsets nothing bounds keep their constraint.
(Possible extension, not done: drop pairs SeaM decided different, with the
premise stated and reported as for the rewriter.)

### The ownership drop (`--ack-own`)

Status: implemented, off by default. It rests on A-INB and on A-OWN, neither
of which `--seam` assumes by itself (its premises are A-SEP, A-NULL and
A-NOWRAP); they are used only when this switch is on, and counted.

**A-OWN (ownership):** no access through one object touches another
object's cells, reads included. A-SEP is stated for write / write and write
/ read pairs (SeaM.md, "Soundness stance"); an Ackermann pair is two reads,
which A-SEP does not separate: two reads through a and b past their ends can
meet in a gap cell. A-SEP does not imply A-OWN; A-INB of every access does
(placement's objects are disjoint), so the bounds check of the program that
discharges A-INB discharges A-OWN too.

The rule. A read index is in object a when every address the selects at it
had before the boundary (`AccessOrigins`, or every leaf of an ite of
addresses, a pointer phi) is a pointer into a, a registered object of
nonzero size. Not in an object: an index with a select of a shadow memory
(keyed by object identity, not an access), more than 8 addresses, an
address with no view or a view of no registered object (a loaded pointer),
leaves in two objects, size 0. A read the lowering merged from several index
terms is in a only if every one of them is. Two reads in different objects a
and b get no constraint: under the A-INB of the access the read in a stands
for, its index is a cell of a, and under A-OWN the read through b does not
touch a cell of a, so the indices differ. Two reads of one object, or with a
read in no known object, keep their constraint.

Unlike `--narrow-offsets`, which takes only a load's own reads (its rewrite
of the index needs the use lemma's one case), the drop also takes the selects
the boundary made (`vc.narrow-skip-not-own`, `-shared-boundary` there): it
does not change an index, it only needs the index to be a cell of its object
whenever the read's value matters. Which A-INB each kind of select rests on:

- a load's own read (a byte of `[q, q + units)` for a load at q): the A-INB
  of that load;
- store-map expansion (`ite(q = s, v, select(M, q))`, a load reading through
  stores the boundary expands) and the reads of a memhavoc array or of a
  memory operation's destination outside its range: the select is at the
  load's own address q; the A-INB of the load;
- wide expansion (`--seam-wide`, the bytes `q + k`, k < units, of a wide
  load): the A-INB of the wide load, whose access covers them;
- a memory operation's source read (`memmove` / `mcopy`: `select(M, s + (p -
  d))`, chosen when the read address p is in the destination range `[d, d +
  len)`): the A-INB of the operation's source access, `[s, s + len)` in the
  source object; when the guard holds, `s + (p - d)` is a byte of it. The
  view of the index (base s's object) is that access's provenance.

In each case the value of the select matters only where the access that made
it executes and its guard holds (the same argument SeaM's rewrites of a load
use); then its index is a cell of the object. Where it does not, the index is
unconstrained and may meet another object's read: the model check below
catches that (no answer). `vc.ack-own-indices` counts the indices in one
object, `vc.ack-own-boundary-indices` those of them with a select the
boundary made.

Where: after every other rule (the exact drops, the decider and the stride
rule), on the index the lowering compares (with `--narrow-offsets` too: two
narrowed reads of different objects are compared at w bits, and the drop
applies to them). It needs the premises on (`vc.ack-own: off-no-premises`
under `--seam-premises=0` or the out-of-object fallback) and the lowering
(`off-no-ackermann`).

Soundness. Unsat: dropping a constraint only weakens the lowered query, so
an unsat holds with no premise at all. Sat: `AckermannModel` compares every
read with the first read at its index value (Model reconstruction, below).
A model that puts two reads whose constraint this rule dropped at one index
with different cells breaks the A-INB of an access or A-OWN, or puts a read
whose access does not execute (or whose guard is false) at another object's
cell; it is counted `own-broken=` on the `vc.ackermann-model:` line and in
`inconsistent=`. A model in which no dropped pair collides is a model of the
original query. A colliding one is not, but its counterexample may still be
a run of the program (counterexamples are runs):

- if every inconsistency of the model is such a pair (`inconsistent =
  own-broken`) and the replayed run fails the assertion with every value
  validated (`replay: assert-failed`, `mismatches=0`), the answer is that
  run: sat, status 0, `own-excused=<n>` on the `vc.ackermann-model:` line
  and the warning `vc.ack-own-collision-excused`. The colliding reads are
  ones the run does not make (a load that does not execute, a memory
  operation's source read whose guard is false), or validation would have
  seen the cell differ;
- otherwise `own-excused=0`, error `vc.ackermann-inconsistent`, status 2:
  the replay does not fail the assertion, a value mismatches, the model has
  another inconsistency (a lowering bug), or under `--ptr-companion=check`
  (its claim is not an assertion failure). A replay that leaves an object
  takes precedence (status 4, `vc.seam-inb-spurious`), as do the other
  replay errors.

Every sat of `sea-vc` replays (no switch turns the replay off). Under
`--reach` the model hook does not see the replay's verdict: a collision
there stays status 2. The run is not repeated without the rule (unlike the
stride rule).

Accounting:
- No `assume.*` line: an unsat answer needs no premise (dropping constraints
  only adds models). A-INB of one read and A-OWN matter for the sat
  answers, which the replay check and the `own-broken` / excused handling
  cover; the dropped pairs are counted by `vc.ackermann-own-pairs`.
- `vc.ackermann-own-reads` (reads in one known object), `-own-pairs`
  (dropped), `-own-same-object` (two reads of one object, kept),
  `-own-not-eligible` (a read in no known object, kept); the last two count
  the pairs that reach the rule.

Measured (`--lower --seam --ack-own`, z3, one run each): nki
`sweep-scatter-N2-D5-K3-pmax2-fmax1-good` 2442 pairs dropped, 406
constraints left (2848 without the switch), unsat as without it.
Of its 262 read indices 242 are selects the boundary made (the job has 21
memmoves); with a load's own reads only the rule dropped nothing. Its sat
siblings `-mut-src-k0`, `-mut-drop-existing`, `-nopre-range`,
`-nopre-unique` replay and validate with `own-broken=0`; `-mut-store-d0`
replays and validates too, but its model puts 6 dropped pairs at one index
(memmove source reads whose guard is false, indices far outside every
object): excused, sat with `own-excused=6`.

Reported (`vc.ackermann-*`, with `--brunch-stats` as BRUNCH_STAT):
arrays lowered / kept, distinct reads, pairs = sum of n(n-1)/2, numeral,
offset, residue, leaf, decider and stride pairs, constraints added (= pairs
minus the dropped ones).

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

## Narrowed offsets (`--narrow-offsets`)

Status: implemented, off by default. It rests on A-INB, which `--seam`
does not assume by itself (its premises are A-SEP, A-NULL and A-NOWRAP):
A-INB is used only when this switch is on, and it is counted and checked
like the other premises (below).

### Why

After `--vc-defs=hidden` names the shared address sums
([SumNames.md](SumNames.md)), what is left of a parser's query is the pairs
themselves: one 64-bit address equality per constraint, about 210
bit-blasted expressions each. On firedancer's `txn-pf-v1-writes` (8101
constraints) about 97% of them compare two reads of one object, the
1232-byte payload. An offset into that object needs 11 bits, not 64.

### What it does

Where: in the lowering, on the index of each read, before the pairs are
formed (`Ackermannizer::setNarrow`, a `NarrowTable` filled by sea-vc).

A read index i is eligible when every select at it is an own read of a
program load: a select the load's encoding left at the load's own address
(a byte of [q, q + units) for a load at q: `seam::Rules::noteOwnReads`,
called by `SeaOpSem::memLoad` after the rewriting, which keeps the load's
address as the index of every select it leaves). Selects the boundary makes
(store-map expansion, the reads of a memmove's source, wide expansion) are
not own (`vc.narrow-skip-not-own`). A select that is both, one node by
hash-consing (a select reachable as a value of a memmove's source read in
normal form, `ite(j = i, v, select(m, a + i))` included; any select a
boundary translation or wide expansion ever built, memo hits included), is
not narrowed either (`vc.narrow-skip-shared-boundary`): only a load's own
reads are, so the use lemma has one case. The reads of wide
loads (`--seam-wide`, expanded into bytes at the boundary) are not tagged
and not narrowed (conservative). Further, every read address of the index (`AccessOrigins`, the stride
rule's record, now keeping every address), or every leaf of an ite of them
(a pointer phi), points into one object of size N > 0, and no select at the
index reads a shadow memory. Let w be the index width (64 on the
jobs measured) and k the least width with `2^k >= N`. After the boundary
the index is `base + o` (a flat sum with the object's constant as one
summand, or an ite of such sums); base and o both have width w. The
read's index becomes

    i' = base + zext(low_k(o))

where `low_k(o)` is computed at k bits from truncated operands
(`include/sea/Expr/LowBits.hh`: truncation is a ring homomorphism for +,
-, *, negation; it commutes with the bitwise operators, ite and shifts by a
numeral; zext, sext, concat and extract are read off; anything else is an
`extract`). So the cursor's sums become k-bit sums, and the bit-blast
shrinks; extracting the low bits of a w-bit sum would not.

The lowering then works on i' as on any index (read sharing, the exact
drops), with one addition: two narrowed reads of one object compare
`base + zext(t_k) = base + zext(t_l)`, which holds iff `t_k = t_l`; the
pair gets the k-bit constraint `t_k = t_l -> r_k = r_l`, after the exact
drops are tried on the k-bit forms (same symbolic part with different
numerals modulo 2^k, residues, leaves, the difference decider at k bits). A
pair with one narrowed read and another (a read of another object, or one not eligible) keeps a w-bit
equality, over i'. The stride rule is not applied to a narrowed read.

With `--vc-defs`, a narrowed read's index sum is its k-bit offset
`low_k(o)`: SumNames names the k-bit prefix chain (the names are k bits
wide). The w-bit sums stay wherever else the query uses them.

### Soundness

Each narrowed read is a rewrite of its own index, justified by the A-INB of
its own access, the same way a SeaM rewrite of a load is justified by the
premises of the accesses it compares. The use lemma: the value of an own
read is used only where its load executes; the load executing, A-INB gives
`0 <= i - base < N` for the byte it reads. If the access executes and stays in
its object (A-INB), its offset o (the w-bit value of i - base, so o mod
2^w) lies in [0, N), so `low_k(o) = o` (`2^k >= N`) and i' = i. If the
access does not execute, its value is not used by the query (the read
stands for a load the run does not do; every SeaM rewrite of a load rests
on the same argument), so its index does not matter: the cells are
A(i') for every read, which equals A(i) for every read that executes. The
lowered query is then the Ackermann lowering of the query with i' in place
of i: functional consistency over the i', so every model reconstruction
(`AckermannModel`, keyed by the values of i') is consistent.

A-NOWRAP is not needed: the w-bit address arithmetic is modular already,
and A-INB constrains the wrapped value o mod 2^w. `base + o = base + o'`
(mod 2^w) iff `o = o'` (mod 2^w), and for o, o' in [0, N) that is
`low_k(o) = low_k(o')`.

Why per read and not per pair: replacing only the pair equality
`i_k = i_l` by `low_k(o_k) = low_k(o_l)` would be stronger than the original
constraint only when an offset is out of range, which happens for a read on
a path the run does not take (a parser reads past a length it has not
checked yet on the other branch). Such a read's w-bit pairs with other
objects' reads would remain, and the two kinds of constraints together can
equate the cells of two in-bounds reads at different addresses (a k-bit
pair to one, a w-bit pair to the other through the out-of-range read). The
per-read representative has no such mix: every constraint of a narrowed
read is over i'.

Unsat holds for the executions in which every narrowed read's access stays
in its object. Two loads whose A-INB conditions cannot hold together (p + i
and p + i + 16 of a 16-byte object) make the answer unsat vacuously, where
the VC without the switch is sat: the program has a memory-safety error,
which the discharge of A-INB (a bounds check of the program) reports;
`--narrow-check` does not (test/VC/narrow/adversarial.test, t3).

A sat model whose run executes a narrowed load out of its object does not
replay at that load: the run reads the cell at i, the model the one at i'.
Model evaluation (`AckermannModel`) gives a narrowed select the cell the
query read, so validation reports the difference; then sea-vc evaluates i
and i' of every narrowed select in the model, and if some differ it prints
`seam.narrow-inb-violation: reads=<n> first: <index> = <v>, narrowed <v'>`
and exits 4 (`vc.narrow-inb-spurious`), as for a broken premise. (The
abstract program's run, which `vc.seam-inb-spurious` uses, stops at the
first read of a nondet memory and decides nothing here.)

What it does not apply to:
- pairs of reads of different objects, or with a read whose base is
  unknown: their constraint is the w-bit address equality (no A-SEP is
  used here);
- a pointer comparison of the program (`PTR_EQ`, `sea.ptr.cmp`, a
  one-past-the-end pointer): not a read index, untouched;
- every other use of the offsets: only the read's index changes; the
  program's arithmetic on the cursor stays w-bit;
- runs without the premises (`--seam-premises=0`, the out-of-object
  fallback) and runs without the Ackermann lowering (`vc.narrow:
  off-no-premises` / `off-no-ackermann`); `--seam-words` is refused.

### Machine-checked

The lemmas of this section are proved in
seam-lean (module `SeaM/VCDefs`; the proofs are to be published), over
a model of the index terms at one width w:

- B1: `low_k(e) = extract(k-1, 0, e)` for every term the rebuild handles
  and every k (each operator case of `LowBits`, the fallback included).
- B2: splitting the index into `base + S` and rebuilding it keeps its
  value; S has the width w of the base.
- B3: for k <= w, two narrowed indices of one object are equal iff their
  k-bit offsets are.
- B4: the exact drops (offsets, residues, leaves) at k bits are carried
  over to the narrowed indices by B3.
- B6: the k-bit constraint of a pair is functional consistency over the
  narrowed indices, so the Ackermann results apply to them, read sharing
  included.
- B7: the narrow-check obligation `S <u 2^k` gives i' = i (by B1 and B2).
- B5, the use lemma: the query over i is satisfiable iff the query over i'
  is, with A-INB read per execution (every read whose guard holds is in
  bounds). It is proved under two facts of the VC that the model does not
  contain, taken as hypotheses: the query does not use the value of a
  read that does not execute, and an executing read's narrowed index
  depends only on earlier reads that execute (dominance). Both rest on
  one property of the C++, not checked: every select a load's encoding
  leaves is at the load's own address or is noted as a boundary read
  (`vc.narrow-skip-not-own`, `vc.narrow-skip-shared-boundary`).

Not modelled: that the narrow-check obligation follows from A-INB, and the
n-ary regrouping of the index into `base + S` (its ite case is proved).

### Accounting

- `assume.a-inb: narrow-reads=<n> narrow-pairs=<m>`: n reads rest on the
  A-INB of their access, m pairs of them are compared at k bits.
- `vc.narrow-*`: indices seen, eligible, skipped by reason (`not-own`,
  `shared-boundary`, `no-object`, `shadow`, `size`, `many`, `shape`), reads narrowed, their
  objects, the largest k, k-bit constraints and the sum of their widths,
  mixed (w-bit) constraints with one narrowed read.
- `--seam-check` / `--narrow-check`: what is checked per run and what
  rests on a lemma are split.
  - Per run, for each narrowed index i = base + S (the lowering's record of
    the selects it narrowed; S has the index width w, as base): the premise bound, `layout && inBounds(a, 1)
    for each read address a && !(S <u 2^k)` must be unsat. inBounds(a, 1)
    is the weakest hypothesis: what the executing load's A-INB gives for
    the byte it reads. It proves `A-INB => S < 2^k`, never A-INB itself.
    First syntactically: after each sum's summands are put in one order,
    the bound is usually a premise conjunct itself, `bvule(S, c)` with
    c < 2^k (A-INB's `off <= size - units` of a load at base + S), also
    through ites of one condition on both sides (a pointer phi); counted
    `syntactic=`. Otherwise one query per index on a fresh solver (released
    after it), the reads as free constants (a sat answer is checked again
    with the reads), z3's default configuration, bounded by z3's resource
    limit (`--narrow-check-rlimit`, default 50,000,000: untuned; 64-bit as the
    other rlimit flags, z3 enforces at most 4294967295 per check), not by
    time.
  - The lemma: for S < 2^k, i' = base + zext(low_k(S)) = i, since the k-bit
    rebuild low_k(S) is extract(k-1, 0, S) (LowBits: a ring homomorphism
    and the other identities in its header). It is checked by its unit test
    (random terms, every k, z3) and machine-checked in seam-lean (B1, B2,
    B7 above); it is not checked per run.
  - Invalid: exit 5, as an invalid rule firing. Unknown or unexpressible
    (shape, premise): a `narrow-check-unknown:` / `-unexpressible:` line
    (the index printed to 300 characters; `reason=rlimit` on an unknown
    that exhausted the rlimit); with `--narrow-check` the run
    exits 7 (`vc.narrow-check-unproven`), since the narrowing of the answer
    is then not discharged; under `--seam-check` alone the lines are printed
    too and, as its other unknowns, they leave the exit status as it is. An eligible index the lowering did not narrow is
    listed (`narrow-check-not-narrowed:`).
  - `--narrow-check` runs without the rule trail (also with `--vcgen-only`):
    on the firedancer jobs `--seam-check` did not finish generating the VC
    in 13 minutes (the trail of every firing, 22-29 GB).
  - [OBS] Measured (`--vcgen-only --narrow-check`, one run each):
    `txn-pf-v1-writes` 141 of 141 valid (131 syntactic), 1.5 s, 92 MB peak;
    `txn-reach-v0-alt` 317 of 317 valid (275 syntactic), 3.9 s, 163 MB.
  - [OBS] Why the first versions failed there: z3's in-process incremental
    solver did not relate `bvule(S, c)` to `bvult(S, c + 1)` and bit-blasted
    the cursor's sum S (11-60 s per obligation; with the 50M rlimit the run
    passed 20 minutes), where the z3 CLI's preprocessing proves the dumped
    obligation at once; and an obligation that timed out was reported with
    its index printed in full: Expr's printer writes a DAG as a tree,
    exponential for the cursor's shared sums, which is what ran out of
    memory. The lines now print a bounded prefix (300 characters), and the
    bound is discharged syntactically first.

### Measured

Two firedancer jobs, `--lower --seam --widen-memory=4 --vc-defs=hidden`,
without and with `--narrow-offsets`; z3 5.3.3 on the `--smt2` dumps
(`-v:10`, seeds 0-3 as `sat.random_seed` = `smt.random_seed`, timeout
900 s, three runs at a time on a shared machine: the times are noisy).

| job | constraints (k-bit) | z3 bit-blast | aig | z3 seeds 0-3 (s) |
|---|---|---|---|---|
| pf-v1-writes (unsat), off | 8101 (0) | 2.27M | no | 80, 46, 80, 43 |
| pf-v1-writes, on (k = 11, 12) | 8111 (7983) | 1.15M | yes (0.91M) | 37, 97, 63, 57 |
| reach-v0-alt (sat), off | 39287 (0) | 8.94M | no | 169, 125, 171, 134 |
| reach-v0-alt, on (k = 10) | 39232 (39232) | 2.66M | no | 94, 282, 645, 323 |

- The bit-blast halves on the unsat job and shrinks 3.4x on the sat job;
  z3's post-blast aig step now runs on the unsat job. Solve times do not
  follow: no change beyond the seed spread on the unsat job, and slower
  with a wide spread on the sat job.
- [OBS] On the sat job the narrowed query is the smaller one by every
  count z3 prints (SAT clauses 1.8M vs 5.7M, memory 1.46 vs 3.6 GB), and
  the slow seeds are search: seed 2 needed 593k conflicts against 85k for
  the baseline's seed 0. Hypothesis: the k-bit offsets change the
  decision order on the cursor's bytes; one sat job and four seeds do
  not separate that from luck.
- Of the k-bit pairs on `txn-pf-v1-writes`, 128 constraints stay mixed
  (64-bit): one read index is not in one object. On `txn-reach-v0-alt`
  every read is narrowed once ites of addresses in one object are taken.
- Yices and Bitwuzla: not measured yet.

## Lazy constraints (`--ack-relax`)

Status: implemented, off by default; a diagnosis mode. The eager lowering
above stays the default. The mode asks which constraints an answer needs;
it is not a replacement for the eager constraints.

`--ack-relax=<selector>`; `include/sea/Expr/AckRelax.hh`; tests
`test/VC/ackermann/relax.test`, `unittests/Ackermann` (`relax`).

The candidates R are the constraints the lowering keeps after every drop
above (exact, decider, stride, and the ownership drop when on). A pair a
drop removed is never a candidate, and the loop never adds it.

The selector picks the kept set K of the first round. Implemented: `all`
(K empty, every candidate omitted). The selector is a parsed value so that
others can be added (by cost of the index difference, by drop class, by
location, top-N, a seeded fraction); an unknown selector is an error.

Each round solves the query with K on a fresh solver: the lowered body, the
constraints in K, and with `--vc-defs` every definition of a named sum (the
names are made once, on the whole query; a definition only fixes its name,
so the ones only omitted constraints use change no answer). A fresh solver
per round, not push / pop: z3's incremental solver skips its tactic
pipeline, so the rounds would not measure the query the eager run solves.

- unsat: the answer. Omitting constraints only adds models, so an unsat of
  the relaxed query is an unsat of the whole one. No premise.
- sat: every omitted candidate `lhs = rhs -> r_k = r_l` is evaluated in the
  model (each term once; `lhs`, `rhs` are the compared indices, or the k-bit
  offsets of two narrowed reads). If none is broken, the model satisfies the
  whole lowered query: the answer, with the usual model reconstruction,
  replay and validation. Otherwise every broken candidate joins K and the
  next round runs. A term without a numeral value counts as broken
  (`unevaluated=`), which only adds a constraint.
- unknown: the answer.
- time: `--timeout` bounds the whole loop, not each round. Each round (and
  the final whole-query solve of a fallback) gets what is left of the
  budget. When it is gone, before a round or in a round that came back
  unknown, the loop stops with `vc.ack-relax-stop: timeout`: no further
  solve, no fallback, `result: unknown`, `solve-timeout: true`. The
  `vc.ack-relax-*` lines are printed as for any other stop.

Per-round policy: all broken candidates are added. Each round is a whole
solve, so fewer rounds is the cheaper direction, and the measured broken
sets are small. The kept set may then be larger than a minimal one.
Not tuned; a bounded number per round is an alternative.

Bound: `--ack-relax-rounds=<n>`, default 50 (untuned). After n relaxed
solves without an answer the whole query is solved on the run's solver
(warning `vc.ack-relax-fallback`, `vc.ack-relax-stop: rounds`, a
`vc.ack-relax-round: fallback` line), as the stride rule's retry solves
without the rule.

Model check. After a `consistent` stop every candidate holds in the model,
so `AckermannModel` finds what it finds under the eager lowering:
`inconsistent` can come only from pairs the stride rule or the ownership
drop removed, and those are handled as above (the stride retry, which
starts the rounds again; `own-broken`, `own-excused`).

Backends: z3, Yices and Bitwuzla (Bitwuzla with `--ackermannize=on`, as
for the lowering itself). Conflicts and the resource count per round are
z3's (`conflicts` of the SMT core plus `sat conflicts`); the other backends
report time only. Not with `--smt2`, `--vcgen-only`, `--reach` or a split
(refused). Without the lowering (`--ackermannize=off`, or `auto` that does
not lower) the switch does nothing: `vc.ack-relax: off-no-ackermann`.

Output: `vc.ack-relax: <selector>`, `vc.ack-relax-candidates` (|R|) with the
other counters; after the rounds `vc.ack-relax-stop`, `-rounds`, `-kept`
(|K| of the last relaxed solve), one `vc.ack-relax-round: <i> kept= result=
broken= [unevaluated=] [conflicts= rlimit=] time=` per solve, and
`vc.ack-relax-conflicts` (z3, summed over the solves). `solve-time` is the
time of every solve (with `--timeout`, at most about the timeout). `--ack-relax-smt2=<file>` writes the query of the last
solve (to measure a kept set with the solver binary over seeds).

Not measured yet on the benchmark jobs.

## Model reconstruction

A model of the lowered query has values for every i_k and r_k, none for A
(the solver never saw A). `AckermannModel` (a `solver::Model` wrapping
the solver's) rebuilds A as the finite function

    F_A = { value(i_k) -> value(r_k) }   with default 0

consistent by the constraints (two reads with equal index values have
equal cell values; a pair that is not, `inconsistent`, is a lowering bug,
or under `--ack-own` a model that breaks its premises (`own-broken`, above):
sea-vc reports error `vc.ackermann-inconsistent`, status 2, unless the
replay confirms an `own-broken`-only model, above). Its `eval(e)`
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
- Pair reductions beyond linear forms and the difference decider's
  structural ranges (no ARM, no ranges from assumes, no reasoning on ite
  conditions beyond the identity of the condition node; A-INB only under
  `--narrow-offsets` and `--ack-own`).
- The number of constraints is quadratic in the reads of one array with
  different symbolic parts (array_list_swap: 4753), and it is what makes
  the lowered query slower there for every solver. Fewer constraints need
  more pairs decided different: by ARM / A-INB (stated premise, as for the
  rewriter), or by a different reduction (z3-style lazy lemmas, a
  sorting-based encoding). Not implemented, except the diagnosis mode
  `--ack-relax` above (lazy constraints by whole solves).
