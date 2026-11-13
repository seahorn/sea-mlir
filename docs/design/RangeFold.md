# Range folding

`lib/Transforms/RangeFold.cpp` (`sea-range-fold`), the integer ranges of
the sea value ops in `lib/Dialect/Sea/SeaIntRanges.cpp`, the fact helpers
in `include/sea/Transforms/RangeFold.h`; `sea-vc --lower --range-fold=on`
(`check`: the paranoid job). Tests: `test/Transforms/range-fold.mlir`,
`test/VC/range-fold.mlir`, the oracle `test/Transforms/int-ranges.test`.

Status: implemented, off by default (untuned).

## Why

A harness often bounds an input with an assume and then guards a computed
index with a comparison that the bound already implies. A guarded store
writes row `p` at the destination `select(in_range, lo, sink)` with
`lo = base + sext(slot) * row` and `in_range = (lo >= base) && (lo + row <
end)`, after `assume(0 <= sext(slot) < T)`. The bound implies `in_range`,
but no pass folds it, so every access through the select carries both
addresses, and each read of the memory gets Ackermann constraints against
both. sccp folds only constants and SeaM does no path-condition reasoning.
The pass folds the comparison in the program, before the VC.

## What it folds

Candidates: every `sea.icmp`, every i1 `sea.and` / `sea.or` / `sea.xor`,
and the condition of every `sea.select`. A candidate whose range at its
position is one value is replaced: the icmp or i1 op by `sea.const`, the
select by the arm it takes. An op whose operands are all constants is left
to the folders (canonicalize). canonicalize then folds what the constants
leave: `and`s of constants, asserts and assumes of `true`, branches.

## How a range is computed

Value tracking at fold time, not a
dataflow analysis: the facts that hold depend on the position of the use,
which a global per-value analysis does not give.

The range of a value `v` at the candidate `P` (a `ConstantIntRanges`:
an unsigned and a signed interval):

1. If `v`'s op implements `InferIntRangeInterface` and has integer operands:
   its `inferResultRanges` on the ranges of its operands, computed the same
   way, recursively, up to `max-depth` ops deep (12, untuned; the depth cap
   gives the full range and is counted). Otherwise (a load, a nondet, a
   block argument, a memory, address or pointer op): the full range. There
   is no memory reasoning: a load is a leaf, refined only by facts on that
   same SSA value.
2. Intersected with every fact on `v` whose `sea.assume` dominates `P`
   (`mlir::DominanceInfo::properlyDominates`: same block and before `P`, or
   in a block that dominates `P`'s block). Facts apply at every value of the
   walk, not only at leaves (`sext x` in the motivating case).
3. Tightened: an unsigned interval inside one sign half is the same signed
   interval, and the other way round (`ConstantIntRanges` keeps the two
   apart; `x sge 0 && x slt 6` is a signed fact that `x ult 6` needs as an
   unsigned one).

The walk is memoized per candidate. An empty range (contradictory facts:
`P` is unreachable) stops the candidate without a fold, counted (`empty`).

## Facts

A fact is a simple bound: `sea.assume c` where `c` is `sea.icmp pred x, C`
or `sea.icmp pred C, x` (C a constant, any predicate), or an i1 `sea.and`,
nested, of such conjuncts (a conjunct `true` adds nothing). `eq` gives a
point; `ne` gives an interval only when C is an end of the unsigned or
signed range, otherwise it is a hole and skipped. Every other conjunct
(a disjunction, a comparison of two values, an i1 value, arithmetic inside
the comparison) is not a fact and is counted (`facts-skipped`). A bound on
`sext(y)` or `zext(y)` is also one on `y`, inverted exactly
(`rangeBeforeExt`), so a fact on `sext(slot)` applies where the walk reaches
`slot`.

No per-value cache of facts. Caching a fact as a value's range for all uses
when its assume "dominates every use" is not sound as stated: the assume's
own condition uses the value before the assume, and a candidate there (the
icmp the assume tests) would fold to `true` by its own fact. The dominance
check per fact and candidate is cheap (one dominator-tree query).

## Soundness

Let the assume `A` dominate the candidate `P`. Every execution that reaches
`P` passed `A` with its condition 1, and SSA values do not change, so at `P`
the fact holds of its value. The transfer functions over-approximate sea's
semantics (below), so the range of a value at `P` contains every value it
has at `P`; a range of one value is its value in every execution reaching
`P`, and every use of `P`'s result is dominated by `P`. There is no
circularity: a fact from `A` is used only at points `A` dominates, while
`A`'s condition is computed before `A`.

A fact from a non-dominating assume is never used: a sibling branch's assume
or an assume after the use (such a fact can give a wrong unsat);
`test/Transforms/range-fold.mlir` (@sibling, @def_before_arm,
@later) and `test/VC/range-fold.mlir` (@sibling stays sat and replays) pin
it.

## Transfer functions: upstream, checked against sea's semantics

The sea value ops implement `InferIntRangeInterface` with the upstream
functions of `mlir/Interfaces/Utils/InferIntRangeCommon.h`. Those are
written for `arith`, where some inputs are poison or UB; sea's ops are total
SMT-LIB bit-vector ops. Per op:

| op | range | sea difference handled |
|---|---|---|
| add, sub, mul | inferAdd / inferSub / inferMul | a possible wrap gives the full range |
| shl | inferShl | flags used only when every shift amount is below the width: under a flag upstream saturates a shift by >= width, while sea's `0 << k` keeps the promise and is 0 |
| lshr, ashr | inferShrU / inferShrS | shift by >= width: full range |
| udiv | inferDivU | `x / 0 = ~0`: added when the divisor may be 0 |
| urem | inferRemU | `x % 0 = x`: the dividend's range added when the divisor may be 0 |
| sdiv, srem | inferDivS / inferRemS | a divisor that may be 0: full range already; `INT_MIN / -1` is an overflow: full range |
| and, or, xor, umin, umax, smin, smax | upstream | total |
| zext, sext, trunc | extUIRange / extSIRange / truncRange | total |
| icmp | evaluatePred | 0 or 1 when the ranges decide, else [0, 1] |
| select | the arm's range if the condition is one value, else the union | integer results only |
| const | the value | |

The oracle `sea-int-ranges-test` (`test/Transforms/int-ranges.test`) checks
every op against `evalPureOp` at widths 1-3: for every tuple of operand
ranges (every unsigned and signed interval, and intersections of the two:
all of them at widths 1-2, every 7th pair at width 3) and every tuple of
values in them, the value is in the inferred range.
It also checks the fact helpers (the bound of each predicate and constant,
`swapPredicate`, `rangeBeforeExt`, `tightenRange`) up to width 4. Without
the `udiv` / `urem` / `shl` adjustments it fails.

`nsw` / `nuw` are binding semantics in sea-mlir, not assumptions: a front
end that does not trust its flags strips them (`sea-wrap-flags mode=drop`);
otherwise sea-mlir would need poison semantics. The VC encoding ignores the
flags (the value wraps, as in SeaHorn), an over-approximation of the flag
semantics. The upstream functions use the flags when the op carries them
(add, sub, mul, shl), so a range fold can be stronger than the encoding:
`test/VC/range-fold.mlir` @wrap (x in [0, 10], `x + 250 nuw`) is sat
without the fold (the encoding wraps) and unsat with it (the flag says no
wrap), the intended answer. Folds whose walk met a flagged op are counted
(`wrap-flags`, informational, an over-count: the flag may not have
mattered).

## The paranoid job: `--range-fold=check`

`mode=check` folds nothing: after each candidate it would fold (before a
select) it inserts `sea.assert (icmp eq v, k)` for the settled value `k`,
and drops the program's own asserts (reach-site markers kept, as
`sea-check-align`). `sea-vc --lower --range-fold=check` answers unsat iff
every fold the pass would make is valid on every execution (within the
bound) of the VC encoding, which ignores nsw / nuw: a fold that uses a flag
(`wrap-flags`) can fail the check job only on executions the flag rules
out (@wrap: check sat). Not combined with `--check-align` or `--check-ptr-index`
(`vc.check-jobs`). Both modes decide every candidate first, on the
unmodified program, and only then fold or insert the checks, so the check
job covers exactly the folds a fold run makes (deciding and folding in one
sweep would let a later candidate see an earlier fold's constant).

## Position in the lowering

In `buildVCLoweringPipeline` (`lib/Transforms/Passes.cpp`, step 6'), after
`sea-expand-intrinsics` and right before the final canonicalize, preceded by
its own cse (with `--lower-opt`). There it sees:

- the program after sccp and cse (constants folded, one value per
  computation, so a fact on one copy of `sext x` is on every use);
- the comparisons the lowerings made: expanded min / max intrinsics,
  fat-pointer bounds, the bound assumes of `sea-expand-memset`;
- and canonicalize runs right after it, folding the constants, assumes,
  asserts and branches it leaves; the second `sea-merge-asserts` restores
  BMC form after the check job's asserts.

Earlier (right after the first cse) it would see fewer comparisons and
leave the clean-up to the same canonicalize.

## Counters

On the program as the attribute `sea.range_fold`, printed by sea-vc as
`vc.range-fold: mode=on|check ...` and by the log `lower.range-fold`
(level 1 per run, level 2 per fold with its location):

- `icmp`, `bool`, `select`: folds by kind; `checks`: asserts of the check
  job;
- `queries`: candidates looked at; `facts`: simple bounds found (before the
  steps through extensions); `facts-skipped`: conjuncts that are not simple
  bounds; `facts-used`: fact intersections that narrowed a range in a fold;
  `no-facts`: folds that used no fact (the ranges alone decide);
- `depth-cap`: candidates whose walk hit `max-depth`; `empty`: candidates
  under contradictory facts; `wrap-flags`: folds that met a flagged op.

## Loads of the same cell

No memory reasoning: when the assume bounds one load of a cell and the
guard uses another load of the same cell (from a later memory version), the
two are different SSA values and the fact does not reach the guard. The
fold fires when the harness loads the bounded value once and reuses it, or
after a load-over-store pass.

## Open

- Default off: untuned; on only if the corpus gates show only query
  reductions with identical verdicts.
- Facts from branch conditions (the true edge of `cond_br c` dominates a
  block where `c` holds) are not used: the BMC-form programs keep such
  guards as branches, not assumes.
- `max-depth` 12 is untuned (long `and` chains of check accumulators hit
  it). A range the cap cut is not memoized within a candidate's walk, so a
  value reached first at the cap is computed again when reached shallower;
  there is no cache across candidates.
- A use-point fold (a fact between a value's definition and one of its uses
  folds that use only) is not done: the point is the candidate's
  definition.
