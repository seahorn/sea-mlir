# Case splitting of the VC

`include/sea/VC/Split.h`, `lib/VC/Split.cpp`; `sea-vc --split`,
`--split-leaves`, `--split-auto`, `--split-candidates`, and the report
`--ite-stats` (`sea-vc --agent`, section SPLIT and the `ite.*` lines); tests
`test/VC/split/`, `test/VC/ite-stats.mlir`.

## Why

A query that times out is one formula; nothing says which case of it is
hard. A split answers several smaller queries, each a case of the
original, and reports each part's counters (open alias questions,
Ackermann pairs, `row.*` firings, query size), answer and time. It is
infrastructure for experiments and for debugging timeouts; it has no
automatic strategy beyond a simple, optional heuristic.

SeaM does no path-condition reasoning: an alias question whose answer
depends on a branch stays open. A split is an explicit, counted, top-level
case split outside the rewriter: SeaM rewrites each part with the case
fixed.

## Construction

Shannon cofactoring. For a term `t` of the VC and values `v_1 .. v_n` with
conditions `c_1 .. c_n` such that

- (E) `c_1 \/ ... \/ c_n` is valid (exhaustive), and
- (X) `c_j => t = v_j` is valid for each j (exact),

the query `Q` is satisfiable iff some part `Q_j = Q[t := v_j] /\ c_j` is:
a model of `Q` satisfies some `c_j` (E) and then `t = v_j` (X), so it is a
model of `Q_j`; a model of `Q_j` satisfies `c_j`, so `t = v_j` (X) and it
is a model of `Q`. The substitution may replace any subset of the
occurrences of `t`. So: unsat iff every part is unsat; sat iff some part
is.

Targets:

- an i1 value with term `t`: `v = 1, 0`, `c = (t = v)`;
- a block guard `g` (a Bool constant of VCGen): `v = true, false`,
  `c = g` / `!g` (the entry block has none: refused);
- an address whose term is an ite tree, or `ptr-from-bits` of one: one case
  per distinct leaf `l` (hash-consed identity, first-occurrence order),
  `v = l` (`ptr-from-bits(l)`, normalized by the rewriter), `c` = the
  disjunction over the paths to `l` of the conjunction of the ite
  conditions on the path;
- an integer (`iN`, N > 1) whose term is an ite tree: the same, `v = l`.
  A value merged over paths (a column index chosen by a branch, a
  coefficient picked under `j == column`) is an ite of its per-path
  values over the block guards; a product by it is a product by an ite,
  which z3 bit-blasts as a multiplier where a product by a constant
  cancels at the word level (a reduced kernel: 9-20 s whole, about 0.1 s
  per case). Splitting on the merged integer gives each part one value.
  The `@<file>:<line>` selector also matches block arguments (a join's
  merged value), for addresses as well (selector policy below).

## Where: during the encoding

SeaM rewrites each load and store as the op is encoded (`SeaOpSem` calls
the rewriter), so there is no unrewritten VC to substitute into. The split
happens at the definition: `OpSemContext::defHook` (after each op
of a block, and after the block arguments of a join) and
`OpSemContext::guardHook` (when VCGen creates a block guard). The hook
replaces the target's value in the symbolic store; every later use reads
the case's value, and SeaM rewrites those uses with it. The target's own
term (`t`) is as the encoding built it, rewritten. The part's condition is
conjoined to the query before its normalization. With no split option the
hooks are null: no term changes (query byte-identical).

Nested targets: one level per target the encoding meets, in encoding
order, each under the cases chosen before it (the tree is not simplified under
the parent's case, so a nested level may yield vacuous parts, whose
condition is false under the parent's; they count toward
`--split-max-parts`). A run takes the choices of
its first levels (the prefix) and case 0 below; its levels give the
siblings. Parts are enumerated depth first, one encoding per part. A
level whose case count does not cover its planned choice stops the split
(`vc.split-unstable`): the encoding is assumed deterministic, and this is
where that assumption would show.

## Checks

`--split-check` (default on) states (E) for every level of a run and (X)
for its chosen case, on the solver's terms, and asks z3 under an rlimit
(an integer level compares by `=`, an address level by address). For
`--split-leaves`, (X) holds at the pointer level by construction (the value
is a leaf of the term's ite tree, or the rewriter's normal form of
ptr-from-bits of one; checked structurally, counted as `leaf-structural`,
an error `vc.split-target` if it fails); the z3 check compares addresses
only. Every case of every level is the chosen case of
some part, so a complete split checks (X) for all of them. A failed check
is exit 5 (`vc.split-check-failed`); an unknown is a warning
(`vc.split-check-unknown`) and the construction is not checked. The
`split.verdict` record carries `check-unknown=<n>` (levels whose check was
unknown or unexpressible) and `check=on|off`, so an incomplete check is
visible in the verdict. The
hidden `--x-split-plant=drop-case|wrong-value` plants the two bugs; the
tests check that the check catches them.

## The heuristic (`--split-auto`)

A pick run (encoding, SeaM, census; no solver) records each address
value whose term is an ite tree, and counts the open alias questions
(`IS_OVERLAP` left in the normalized query) whose pointer arguments
contain the value's term or one of its leaves that has no object (SeaM's
leaf-pair decisions split an ite pointer, so the open question holds the
unknown leaf, not the ite). Ties: more leaves first. The first candidate
with two or more leaves and at least one question is split on its leaves.
The attribution over-counts (an unknown leaf behind several ites counts
for each of them): it is a ranking for experiments, not a strategy.

## Finding targets (`--split-candidates`)

The names of the lowered program are not the source's, so a user who
wants to split by hand first needs a list, and the program those names
belong to. `--print-lowered=<file>` writes the program the VC encodes
(after the lowering and every other pass of sea-vc), with its locations
inline, named as `sea::ValueNames` names it: the names of the selectors
and of the `split.*` lines (a nondet result is `nd.<site>` in the VC). The
run goes on; with `--vcgen-only` it solves nothing. `sea-opt
--sea-vc-lower` is not that program: its options differ from those a
sea-vc run derives (`--seam` skips `sea-expand-memset`, for one) and it
prints without NameLoc prefixes, so its numbers name other values. The
written file is input to sea-vc without `--lower`, with the same names
(printing it again gives the same file).

A selector `@<file>:<line>[:<col>]` takes its matches by kind preference,
so that every selector that named one value before integers and block
arguments were targets still names it: `--split-leaves` takes address op
results, else address block arguments, else integer op results, else
integer block arguments; `--split` takes i1 op results, else i1 block
arguments (passes such as sea-merge-asserts, sea-lower-box-all and
sea-sink-assumes create i1 block arguments with op locations, and the
fat-pointer, struct and tracking lowerings copy block-argument locations to
their companions). Only the first non-empty class counts; `:<col>` keeps the
matches at that column.
Within it, a selector that matches several values is an error that
lists every match (up to 32, then a count) with its op, type and column;
one that matches nothing lists the results of other types on that line
and the nearest lines with results of the kind (three on each side), or,
when no op has a position in such a file, the files that occur.

`--split-candidates` runs the
pick run of `--split-auto` (the same census, `SplitCensus`), lists, and
stops: no split, no solver, `result: none`. It is refused with a split
(`--split`, `--split-leaves`, `--split-auto`): listing and splitting are
separate runs, so the list never depends on a split.

- `split.leaves-candidate`: every ite-valued address, in `--split-auto`'s
  order (that ranking and its pick rule are unchanged; with no open
  question at all, by the read indices below). Besides
  `open-aliases` it gives `unknown-leaves` (leaves without an object),
  `direct` (questions whose pointers hold the term itself, not only an
  unknown leaf of it) and `own` (questions no other candidate is behind).
  When one unknown leaf sits behind several ites, they tie on
  `open-aliases`; `own` below `open-aliases` says the count is shared, and
  `direct` tells the ites in the question apart from those that only share
  the leaf.
- `split.bool-candidate`: i1 values and block guards (the `--split`
  targets) that occur in an ite condition of an open question's pointers,
  or of the term of an address candidate attributed to the question. The
  key of a value is its bv1 term and that term `= 1` (normalized); of a
  guard, its constant. Ranked by those questions (`open-aliases`), then by
  `addrs` (address candidates with it in an ite condition), then encoding
  order. Only candidates behind a question or in a read index (below)
  are listed, one line per term (`same=`, below), at most
  `--split-candidates-max` (default 20, 0: all); the `split.candidates`
  line counts the recorded values, the candidates, and the dropped ones.
  Ites that come from joins have the guards of the blocks before the join
  in their conditions; a branch's two successor guards usually rank
  together, and splitting on either gives the same two cases.

### When no alias question is open: the read indices

A query can be hard with no open alias question at all: its residue is
the Ackermann lowering's pairs of reads of a read-only array, whose
indices are ite terms of conditions the program computes (a select that
picks a row, an in-range guard). Splitting on such a condition fixes the
index in each part, and the pairs that compare it go away. So
`--split-candidates` also translates the normalized query to the
solver's terms (`SeaOpSem::solverTerm`, as the solver run does before
Ackermann) and looks at every distinct array read (`select` with an
index that is not a numeral; the `reads=` of the `split.candidates`
line):

- a Boolean candidate counts a read (`reads=`) when it occurs in an ite
  condition of the read's index, as a whole condition or a part of one,
  and `cond-reads=` when it is a whole condition;
- an address candidate counts a read (`reads=`) when its term occurs in
  the index.

Keys are translated by `solverTerm` too (the boundary translates
node by node, so a subterm of the query becomes the translation of the
subterm). A Boolean candidate is listed when it is behind an open
question or counts a read. Among those with `open-aliases=0` the order
is `reads`, then `cond-reads`, then `covers` (the other candidates in
its Boolean structure: and / or / not / ite and their bv1 forms, down to
the atoms; an and of two compares before the compares), then `addrs`,
then encoding order. With no open question at all, the address
candidates are ordered by `reads`, then leaves (`--split-auto` does not
run this step; its ranking and pick are unchanged).

Two ops can compute the same term (a range check before and after a
store to another object reads the same cell). They are one line: the
value with the most uses that reach a select condition, a branch
condition or an address is named (followed forward through i1
connectives: and, or, xor, i1 selects, i1 block arguments; a compare
reaches the select through the and that is its condition), then the
first in encoding order; the others are in `same=`. They are not the same split target: a split replaces the
value of the named op only, and the uses of the others keep the term
(the part's condition still holds it, so the parts stay exact). Split on
the one whose uses matter (`--print-lowered` shows them), or on all of
them (each is a level).

This attribution is a heuristic. It counts reads, not pairs, and a read
whose index holds a condition need not be in a costly pair; it does not
see a condition that SeaM moved out of the index (a read at an ite
address that SeaM split into an ite of reads).

### Arithmetic: products and quotients by path-merged values

Neither an open alias question nor an array read: a query can be hard
because it multiplies or divides by a value the VC merged over paths.
`--split-candidates` also records every integer value (`iN`, N > 1) whose
term is an ite tree, and looks at every distinct arithmetic node of the
query at the solver boundary (`bvmul`, `bvudiv`, `bvsdiv`, `bvurem`,
`bvsrem`, `bvsmod`, the multiplication overflow predicates; the
`arith-nodes=` of the `split.arith-candidates` line):

- an integer candidate counts a node (`arith=`) when an operand holds its
  term (`direct=`), or an ite whose condition's atoms (below and / or /
  not / ite and their bv1 forms) are all atoms of the conditions of the
  candidate's normalized tree. The second rule is needed because the
  normalization folds a compare of the merged value into its conditions
  (`ite(g, 0, 1) = 0` becomes `g`), so the coefficient picked by
  `j == 0` no longer holds `j`'s term. It over-attributes: every value
  merged over the same paths shares the count;
- a Boolean candidate (the `split.bool-candidate` keys) counts a node when
  it is in an ite condition of an operand, as for the read indices.

`feeds=` counts the other integer candidates whose term, as encoded,
holds this one's (the coefficient and the variable chosen by the index
hold the index): the values a split on it fixes too. One line per
candidate with `arith > 0` (`split.arith-candidate`), ranked by `arith`,
then integers before Booleans (one integer split covers the whole merge,
a Boolean is one decision of it), then `feeds`, then `direct`, then
cases, then encoding order; at most `--split-candidates-max`. Conditions
with more than 64 atoms attribute nothing (an untuned bound on the cost).

### The ite report (`--ite-stats`)

What the triage of a cliff wants before any split: which values the
encoding merged over paths, and where they are used. `--ite-stats`
installs a census on the encoding's hooks (after the split or candidate
hooks, chained, changing nothing) and reports, after the first encoding
of a run and before any solver (so also under `--vcgen-only`):

- values whose term is an ite tree (an address: or `ptr-from-bits` of
  one), by kind: `mem` (memory values), `addr` (addresses), `int`
  (integers in the backward slice of an operand of `mul`, `udiv`, `sdiv`,
  `urem`, `srem`, `umulo`, `smulo`, `sdivo`, through integer and i1 ops and
  block arguments, not through memory: an index used through compares and
  selects counts). Per source location: the values, and the most distinct
  leaves, distinct ite conditions, and block guards among the conditions'
  atoms (`guard-atoms`: a tree over guards is a path merge; one over
  program conditions is a select). The leaves are counted on the DAG, so a
  tree with more paths than `--split-max-paths` is still counted;
- loads whose address term has a free constant (numerals and SeaM's object
  bases are ground), by kind (`ite`: an ite tree or ptr-from-bits of one;
  `from-bits`: ptr-from-bits of anything else, a pointer read back from
  memory; `ite-index`: an ite below the top, an index chosen by a path or
  a select; `symbolic`: other) and location. Under `--seam` the address is
  SeaM's normal form, so a pointer SeaM resolved to an object counts as
  ground.

Not with a split (each part is an encoding). Memory values are counted
only when their term is an ite at the top: under `--seam` a merged memory
may be a store-map over ites, and under `--mem=lambda` a lambda.

### The counts of open questions

`open-aliases` counts the `IS_OVERLAP` nodes of the normalized query:
the questions the attribution distributes. `--seam-stats` counts more
under `seam.alias.open.*`: also the one-object questions that SeaM
reduced to an offset compare (`Rules::aliasReduced`) and that stay open
after normalization (kind `same-object`). The `split.candidates` line
shows those as `same-object-open=`, so `seam.alias.open.all =
open-aliases + same-object-open` on one query; the open in-tests of
memory-operation reads (`seam.alias.open-memop.all`) are
`memop-open=`. Neither is attributed to a candidate: an offset compare
has no ite pointer to split. A query with thousands of
`seam.alias.open.same-object` and `open-aliases=0` is one whose
questions SeaM decided down to offset compares; its residue, if any, is
in those compares and in the Ackermann pairs.

All rankings are heuristics: cheap syntactic counts over the normalized
query, not a measure of how hard a part will be.

### The hint, and long ite trees

A `--seam` run (not a split) whose verdict is `unknown` counts the open
alias questions (`IS_OVERLAP`) of its normalized query, a DAG walk after
the solver (no cost to sat or unsat runs). If there is at least one, it
prints one line, `hint: split: open-aliases=<n>; rerun with
--split-candidates to list split targets (no solver)` (jsonl kind `hint`,
field `open_aliases`), after the result and its reason lines. Without
open questions but with constraints of the Ackermann lowering
(`vc.ackermann-constraints`, already counted by the run) it prints
`hint: split: open-aliases=0 ackermann-constraints=<m>; rerun with
--split-candidates ...` (field `ackermann_constraints`): the read-index
attribution above may list targets. The run itself does not attribute
reads. With neither there is no hint. It changes
no query and no other line. `utils/query-diff.py` compares `--smt2`
output and does not see it; `utils/bench.py` reads `BRUNCH_STAT` lines
and named `key: value` counters of a run (`observed_counter`), and no
counter is named `hint`.

An ite tree with more paths than `--split-max-paths` has no leaf cases:
the candidate is listed (and ranked) with `leaves=0` and
`unknown-leaves=0`, only its term attributes questions, `--split-auto`
never picks it, and the census warns `vc.split-max-paths`.

## Open questions

- The 64-part cap (`--split-max-parts`) is untuned and stays: a query
  with hundreds of guards needs a target whose cases cover them (an
  integer or address merged over them), not more parts.
- Integer candidates with one term are one line (`same=`, the first in
  encoding order named), as the Boolean ones; a split replaces the named
  value only.
- `--ite-stats` keeps at most 256 leaves, conditions and guards per tree
  (memoized per ite node, so a chain of merges costs linear time);
  `capped=` counts the values that reached the cap. Untuned.
- The ranking of `split.arith-candidate` (integers first, then `feeds`,
  then `direct`) is untuned, set on the reduced kernel. Whether the gate
  query of that kernel ranks its merged column first is not measured.
- The atom rule for integer candidates over-attributes along shared
  paths, and the 64-atom bound is untuned.

- The cap of the Boolean list (20) and the order of ties are untuned,
  as is the order of the read-index keys (`reads` before `cond-reads`
  before `covers`), set on one query.
- A candidate with `same=` values is not split with them: the split
  substitutes by value, not by term. Splitting on a term (every value
  with it at once) is not implemented.
- Default of `--split-check`: on; cheap on the measured queries. A target
  whose term is large could make it costly.
- Splitting a leaf that is itself an ite of pointers under `to-bits`
  (reported as `leaf:unknown(ite)`): a second `--split-leaves` level
  cannot name it (it is not a program value).
- Selecting targets by location matches every op result of the kind with
  a position on the line, inlined call sites included; canonicalization
  may merge the op the user means into another one (then the name of the
  lowered program, from `split.candidate` or the error, is the handle).
