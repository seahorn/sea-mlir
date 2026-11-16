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
lose branches they could not take. No premise,
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
lowering, `lib/Transforms/Passes.cpp`), after sccp, cse and the lowerings
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
folded), printed by sea-vc as `vc.assume-fold: uses= by-assume= by-assert= values=
branches= selects= asserts-held= asserts-failed= blocks=` and by the log
`lower.assume-fold` (level 1 per run, level 2 per folded use):

- `uses`: operands replaced; of them `by-assume` / `by-assert`: by the
  fact of an assume / an assert (the first dominating fact in program
  order); `values`: distinct values with a replaced use;
- `branches`, `selects`: cond_br / select ops whose condition was replaced;
- `asserts-held`, `asserts-failed`: asserts whose condition became 1 / 0
  (dropped / direct failures in sea-merge-asserts);
- `blocks`: blocks reachable before the fold and not after, when each
  cond_br on a constant takes its one edge (what canonicalize then
  erases; blocks sccp's constants already cut off are not counted).

The postcondition collects the facts again on the rewritten program, with
a fresh dominance analysis, and checks that no use a dominating check fixes
is left.

Cost: `collectAssumedValues` walks, per check, the values it fixes; an
assert of each prefix of a long `and` chain fixes the whole prefix, so
that case is quadratic in the chain length (not capped).

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
default's. The pipeline `sea-vc-lower{reach=true}` is the reach lowering
(no fold), `reach-fold-facts=true` adds it.
`test/VC/reach/reach-fold-facts.test`.

## Open

- Edge pruning at a join: an assume whose condition is a comparison of a
  block argument with a constant, at the start of the block, makes every
  incoming edge with a constant argument that falsifies it infeasible (the
  130 `ret = 0` edges into `assume(ret != 0)` in the same firedancer job).
  Not done: it rewrites the predecessors' branches (a cond_br edge becomes
  an assume of the other condition), a different transformation; measured
  on that job without this pass it did not change the cost (10,466
  constraints).
- An assume that post-dominates a branch on its value with no assert in
  between makes the other edge infeasible too; such branches are reported
  (`vc.assumed-branches`), not folded.
- Integer equalities (`assume(x == K)`) are not substituted at dominated
  uses; `sea-range-fold` settles comparisons of `x` under such facts.
