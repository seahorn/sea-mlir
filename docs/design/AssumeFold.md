# Assume folding

`lib/Transforms/AssumeFold.cpp` (`sea-assume-fold`), the analysis in
`include/sea/Transforms/AssumeFold.h`; on by default in `sea-vc --lower`,
off with `--lower-disable=assume-fold`. Tests:
`test/Transforms/assume-fold.mlir`, `test/VC/assume-fold.mlir`.

Status: implemented, on by default (exact).

## Why

A harness assumes a property of its input and the code under test then
branches on the same value:

```
%v37 = sea.icmp sgt, %v35, %v36 : i8
sea.assume %v37                      // harness: a legacy payload
...
sea.cond_br %v37, ^bb223, ^bb2       // parser: legacy or V1
```

Before this pass, nothing in the lowering folded the branch: sccp does not
use assumes, and `sea-range-fold` (off by default) folds selects, not
branches. The V1 parser behind `^bb2` cannot execute but stayed in the VC.
In the firedancer job `txn-pf-legacy-mut-maxacct` (`--lower --seam
--widen-memory=4`) it contributed 152 of 225 Ackermann reads and 9,921 of
10,617 Ackermann constraints; with the fold, 73 reads and 696 constraints,
and the solve went from minutes (60 s to over 600 s, by solver seed) to
seconds. The Ackermann constraints carry no block guards, so the reads of
dead blocks cost as much as live ones.

## What it folds

After `sea.assume %c` or `sea.assert %c`, every use of `%c` in an op the
check properly dominates reads the constant 1. An assert is an assume for
the code it dominates: execution continues past it only if it held (the
control-flow promise of an assert). The assert's own condition is not
folded (the check does not dominate itself). The values a check fixes
(`collectAssumedValues`), starting from its condition (true):

- backward: `and a, b` true fixes `a` and `b` true; `or a, b` false fixes
  both false; `xor a, k`, `icmp eq a, k`, `icmp ne a, k` of an i1 `a` and
  a constant `k` fix `a`;
- forward: an i1 user of a fixed value that the value decides: `xor v, k`
  and `icmp eq|ne v, k` (the negations), `and v, w` with `v` false, `or v,
  w` with `v` true.

Edge facts: the true edge of `sea.cond_br %c` fixes `%c` to 1, the false
edge to 0, with the same derivations. A fact holds on entry of a block if
it holds on every edge into it that can execute (its source reachable from
the entry), and then at every point the block dominates, the block itself
included (`collectEdgeFacts`):

- one such edge (every cond_br target of the BMC form, which has no
  critical edges): what that edge fixes;
- several (a join, or a critical edge: at the fold's position
  sea-split-critical-edges has not run yet, and expand-memset adds
  blocks): a fact some edge fixes, kept only if every other edge fixes it
  too or it holds at the end of that edge's source (a check that
  dominates the source's terminator, or the entry fact of a block that
  dominates the source). `@join_no_fold` in the transform test: ^bb3 is
  entered on `%c`'s true edge and from a block where `%c` is 0, so `%c` is
  not fixed there; `@join_agree`: `or %c, %d` is 1 on the true edges of
  `%c` and of `%d`, so in their join;
- a cond_br whose two edges enter one block fixes nothing.

Blocks are visited in reverse post-order, so a source's entry facts are
known before its successors'; the source of a back edge (not in the
lowering, which is loop-free) is visited later and counts with the checks
and the entry facts of blocks already visited (its target's strict
dominators): fewer facts, never a wrong one.

Integer equalities: an i1 fact `icmp eq x, K` = 1 or `icmp ne x, K` = 0
(K a constant on either side, `x` an integer `iN` with N > 1), from any of
the sources above (a check, an edge, a conjunct or negation of one),
fixes `x` to K in the same region: every use of `x` there reads `sea.const
K` (`collectIntFacts`, after the i1 facts; derivation D7 / rule F3 of the
fact-fold rule table). Every kind of use: arithmetic operands, addresses
computed from `x`, values passed to block arguments, strong prints (they
show the same value), operands of ops with nsw / nuw (the flags stay; the
op with constant operands is not folded: such ops never fold). The icmp
that makes the fact is not in its region (it defines the fact's value,
which dominates the source), so it keeps `x`. Integers only: `sea.icmp`
takes integers; an address or pointer equality (`sea.addr.cmp`,
`sea.ptr.cmp`) is never read, because equal addresses do not make two
pointers interchangeable (provenance, docs/design/PtrSort.md; transform
test @addr_eq). Exact for the same reason as the i1 facts: the fact holds
at every execution of the use, and an operand that reads an equal value
changes nothing. Repro (`test/VC/assume-fold.mlir` @int_eq: `assume x ==
5`, then `x * 4` as a store offset): the store's address becomes a
constant, DAG 138 -> 106.

Rounds: a substitution can make a new fact. `assume (x == 5); assume
(icmp eq y, x)`: once x reads 5, the second compare is `icmp eq y, 5`, the
equality y == 5 (likewise on the true edge of a branch on it, and an i1
derivation can pass a new constant). So the pass collects the facts again
and substitutes until a round replaces nothing (`rounds=`; transform test
@int_eq_chain*, VC @int_eq_chain_edge). It terminates: each replacement
turns an operand that is not a `sea.const` into one, and no fact is about
a constant, so there are at most as many replacements as such operands
and at most one round more; the bound is checked at run time
(`sea-assume-fold.postcondition`). Not within the pass: facts that need a
folder first, e.g. `x ult 10` under `x == 5` (`icmp ult 5, 10` becomes the
constant 1 only at canonicalize).

Every use is replaced: branch and select conditions, connective operands,
block-argument values on edges the check dominates, later asserts and
assumes. The pass changes operands only; sea-merge-asserts drops an assert
it made 1 and turns one it made 0 into a direct branch to its failure site,
and canonicalize folds the constant branches, selects and connectives and
erases the blocks no path reaches. The constants have no location (a
replay's last located op stays a program op).

## Exact

Let the check A fix v to b and dominate the user U. Every execution that
reaches U passed A with its condition 1 (an assume blocks the others, a
failed assert ends them); each step of the derivation is a
bit-vector identity (`and a, b = 1` implies `a = b = 1`, `xor a, 1 = 1`
implies `a = 0`, ...), and SSA values do not change, so v = b at U. The
program after the fold has the same executions with the same values up to
their end, and they fail the same asserts; only paths that already end at A
lose branches they could not take. An edge fact is a check placed on the
edge: every execution that enters the block B took one of its executable
edges, on each of which the fact holds (by the branch's condition, or by a
check or entry fact that dominates the edge's source), and an execution at
a point B dominates entered B last with no redefinition of the value since
(a redefinition would pass the value's definition, which dominates B, and
then B again). No premise,
no entry of the assumption ledger, no `assume.*` line. When one assume
fixes a value both ways (a contradiction), every point it dominates is
unreachable and either constant is exact; the first is kept.

A use the assume does not dominate is never folded: a branch before the
assume, or after the join of a sibling that assumes the value (such a fold
can give a wrong unsat; `test/VC/assume-fold.mlir` @sibling stays sat and
replays).

## Where it lives, and why there

A pass of its own, not an extension of `sea-range-fold`:

- `sea-range-fold` is off by default (untuned), and its range walk uses
  the nsw / nuw flags, so its folds can be stronger than the VC encoding
  (docs/design/RangeFold.md, @wrap). This fold has neither property: it is
  exact on the encoding, cheap (one dominance query per use of an assumed
  value), and on by default.
- Its facts are i1 values, not integer bounds; range-fold's facts are
  simple bounds and it folds at a candidate's definition, not at a use.

Position: right before the first `sea-merge-asserts` (step 3' of the
lowering, `lib/Transforms/Passes.cpp`),
after sccp, cse and the lowerings
before it (placement, widening, `sea-expand-memset`, whose bound assumes
it sees). Before the merge, because the simplification must not depend on
it: sea-merge-asserts is a helper of the BMC form (until asserts stay in
place or programs are split at their asserts), and the program's asserts
are the facts. A check of a condition an earlier check fixed becomes
constant here, and sea-merge-asserts drops it (1) or makes it a failure
(0). A first version ran after the merge, on its `assume P` copies of the
asserts: the held assert's merged branch then folded into a marked
`sea.br` and a replay named it as the failing assert (`test/VC/assume-fold.mlir`
@held_negated). It runs with the pipeline's `fold` switch (with sccp): the
lowering without folds (`sea-vc-lower{fold=false}`, the location-matching
oracle) leaves it out. `--lower-disable=assume-fold` leaves it out for an
A/B of the lowering (`utils/cover-diff.py`, `utils/query-diff.py` by
arguments).

The lowerings after the merge (split-wide-access, expand-intrinsics,
range-fold) make no i1 facts the fold would need; a condition that becomes
constant only there is canonicalize's (a merged assert's branch:
SimplifyCondBr drops the marker if it held, keeps it and warns
`fold.assert-false` if it failed).

The dead blocks stay in the program through the other lowerings (placement,
widening) and go at canonicalize. A hypothesis, not measured: erasing them
earlier could let `sea-widen-memory` widen memories that only dead code
accesses unaligned.

## Counters

On the program as the attribute `sea.assume_fold` (only when a use was
folded), printed by sea-vc as `vc.assume-fold: uses= by-assume= by-assert=
by-edge= values= branches= selects= asserts-held= asserts-failed= blocks=
int-uses= int-values=`
and by the log `lower.assume-fold` (level 1 per run, level 2 per folded
use):

- `uses`: operands replaced; of them `by-assume` / `by-assert` /
  `by-edge`: by the fact of an assume / an assert (the first dominating
  one in program order) / of the edges into a dominating block (only
  uses no check fixes: the checks' facts are applied first); `values`:
  distinct values with a replaced use;
- `branches`, `selects`: cond_br / select ops whose condition was replaced;
- `asserts-held`, `asserts-failed`: asserts whose condition became 1 / 0
  (dropped / direct failures in sea-merge-asserts);
- `int-uses`, `int-values`: operands replaced by an integer equality
  (not counted in `uses`; integer equalities are applied after the i1
  facts of each round), distinct integers with a replaced use;
- `rounds`: rounds that replaced a use (1 unless a substitution made a
  new fact);
- `blocks`: blocks reachable before the fold and not after, when each
  cond_br on a constant takes its one edge (what canonicalize then
  erases; blocks sccp's constants already cut off are not counted).

The postcondition collects the facts again (checks and edges, and their
integer equalities) on the rewritten program, with a fresh dominance
analysis, and checks that no use a dominating fact fixes is left.

Effect on the VC size: K also replaces x where it is passed to a block
argument on an edge in the region. If the join's other edges pass x, the
argument was uniform (canonicalize dropped it); now it is a choice between
K and x, an ite over the incoming edges in the VC: one node more (a
reduced join: DAG 50 -> 51). A hypothesis, not checked job by job: this is
the growth of 1-2 nodes in 29 suite jobs at this step. The constant is kept
on purpose: a constant edge argument is what sea-prune-edges reads to find
dead edges.

Cost: `collectAssumedValues` walks, per check, the values it fixes; an
assert of each prefix of a long `and` chain fixes the whole prefix, so
that case is quadratic in the chain length (not capped). The edge facts
derive once per cond_br edge; a join checks each candidate on each other
edge (a dominance query per check of the value and a walk up the
dominator tree). Every round collects all facts again, O(program) per
round; the rounds are at most the longest chain of facts that each need
the previous round's constants, plus one. On the public and private
suites (941 jobs) no job needs a second round (`rounds=1`
wherever the fold replaces a use).

## Select form

LLVM writes a short-circuit `a && b` as the i1 `select a, b, false` and
`a || b` as `select a, true, b` (its `and` / `or` would let poison of `b`
through when `a` decides). sea has no poison: the select and the
connective are the same function (Semantics.cpp; z3: `(ite (= c #b1) x
#b0) = (bvand c x)` and `(ite (= c #b1) #b1 x) = (bvor c x)` over
`(_ BitVec 1)`, unsat negations). `matchConnective` (AssumeFold.h) reads
both forms as `and` / `or`; the derivations above and sea-range-fold's
conjuncts of an assume use it. Selects with both arms constant, the other
constant arms (`select c, false, x`, `select c, x, true`: a negation of
`c`) and non-i1 selects are not connectives.

The program keeps the selects. A first version rewrote them to `and` /
`or` (a pass before the fold, in the canonicalize sense); on the public
and private suites that cost more than it gave: the select folders fold
`select c, false, false` (equal arms) and `select true, x, false` (constant
condition) once a later lowering makes an arm or the condition constant,
`and c, false` and `and true, x` have no folder (identity folders are a
separate step), so 6 stellar-batch / stellar-split jobs kept up to 6
blocks more; the VC grew in every firedancer job (bv1 `bvand` plus the
Bool conversions, about 0.01%); and two stellar-split jobs whose
`--split-leaves=%1575` names a printed SSA number of the lowered program
failed (the rewrite renumbers values).

## What it leaves: `vc.assumed-branches`, `split.assumed-candidate`

After the lowering, sea-vc lists the cond_br and select ops whose condition
is a value an assume fixes (the same analysis, assumes only, at any
position, on the program the VC encodes, with or without `--lower`; the
assumes that sea-merge-asserts leaves in the continue block of each merged
assert's branch are not counted, so the report does not depend on that pass:
they are recognized by their place, the first op of the block the merged
branch's true edge alone enters, with the branch's condition; an assert's own
conjuncts computed before it are not assumed values):
`vc.assumed-branches: branches= selects= values=` (only when there is one).
With the fold on these are the ones it cannot fold (the assume does not
dominate them); with it off, all of them. `--split-candidates` lists each
such value as `split.assumed-candidate: name= assumed=1|0 branches=
selects= loc=` (a `--split` selector, the value the assume fixes it to):
a split on it keeps only the case the assume allows in one part.

## Under `--reach`

The fold is exact for the safety query only. Under `sea-vc --reach` it
would remove targets: a check it makes 1 is erased by canonicalize with
its reach site (unless `--reach-keep-sites`), an assert it makes 1 loses
its failure target (`--reach-failures`), and the blocks it makes dead are
erased with their checks and block ids (also with `--reach-keep-sites`).
So under `--reach` it does not run by default (everything visible: an
unreachable check is reported, not removed). `--reach-fold-facts` runs it
anyway, an opt-in for performance whose targets are a subset of the
default's (an edge-dead block's checks go too: `if (c) { if (c) ... else
{ assume q } }` lists the `assume q` unreachable by default, and not at all
with the flag). The pipeline `sea-vc-lower{reach=true}` is the reach lowering
(no fold), `reach-fold-facts=true` adds it.
`test/VC/reach/reach-fold-facts.test`.

## Open

- Edge pruning at a join (an assume of a comparison of a block argument
  with a constant falsified by an incoming edge's constants) is
  `sea-prune-edges` (docs/design/PruneEdges.md), after the final
  canonicalize; its rounds run this pass's rewrite on the assumes it makes.
- The cond_br's own edge operands: `cond_br %c, ^a(%c), ^b` passes 1,
  but the operand is not under the edge's block and is not folded.
- A join whose edges fix a value by checks only (each sibling assumes
  `%c`) gets no fact: candidates come from cond_br edges.
- An assume that post-dominates a branch on its value with no assert in
  between makes the other edge infeasible too; such branches are reported
  (`vc.assumed-branches`), not folded.
- Integer facts other than equalities (`x ult K`) are not used here;
  `sea-range-fold` settles comparisons under such assumes.
