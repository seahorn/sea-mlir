# Edge pruning

`lib/Transforms/PruneEdges.cpp` (`sea-prune-edges`); on by default in
`sea-vc --lower`, off with `--lower-disable=prune-edges`, never under
`--reach`. Tests: `test/Transforms/prune-edges.mlir`,
`test/VC/prune-edges.mlir`.

Status: implemented, on by default (exact for the safety query).

## Why

SeaHorn ends a function's paths in a join whose first op assumes a flag
the paths carry:

```
^bb7:
  sea.br ^exit(%false : i1)
...
^exit(%flag : i1):
  sea.assume %flag
```

An edge that passes `false` cannot be taken by any execution that goes on,
and neither can the code that only leads to it, but the VC keeps the
guards, values and memory reads of that code. Likewise a branch into a
block that is only `sea.unreachable` (from the program, or from
canonicalize's fold of an assume of constant false) keeps its condition
open. In the firedancer job `txn-pf-legacy-mut-maxacct` 130 edges pass
`ret = 0` into `assume(ret != 0)`.

## What is dead

An edge is dead when no execution that takes it reaches a `sea.assert`
or a `sea.halt`:

- by its arguments: the target block has a `sea.assume` before its first
  assert or halt whose condition is 0 when the edge's constant block
  arguments are substituted. The condition is evaluated through the op
  folders (`evalPureOp`) over the pure ops of the target block that
  compute it (`icmp ne %ret, 0`, ...); values from other blocks count only
  when they are constants;
- by its target: every path from the target blocks (a `sea.unreachable`,
  an assume of constant 0, a dead edge) before an assert or a halt. The
  observing blocks are a least fixpoint backwards from the blocks whose own
  ops reach an assert or a halt.

A program with a cycle among its reachable blocks is left alone: it is
not BMC form and sea-vc refuses it; removing a loop that never reaches a
check would make that unsupported input pass.

The rewrite, at the first dead edge of each path from the entry: a cond_br
with one dead edge becomes `sea.assume` of the other edge's condition
(`xor c, 1` for the false side) followed by a `sea.br`; a branch with no
live edge becomes `sea.unreachable`. The new assume has the branch's
location; the br and the unreachable keep the branch's location and
discardable attributes (block ids).

## Rounds

The new assumes are facts: the rewrite of `sea-assume-fold`
(`foldAssumedUses`) folds the uses they fix, canonicalize folds the
constant branches and assumes and erases the blocks no edge enters, and
merges a block into its one predecessor, which makes block arguments
constant; cse merges what that exposes. Then the analysis runs again.
The pass stops at the first round that prunes nothing. It terminates: a
round removes at least one edge and the clean-ups add none (checked;
`sea-prune-edges.postcondition`). Without any clean-up (`fold`,
`canonicalize`, `cse` all false) it makes one round, and runs the analysis
again as its postcondition. The clean-ups follow the lowering's switches:
no assume-fold rewrite with `--lower-disable=assume-fold`, no canonicalize
/ cse with `--lower-opt=false` or `--lower-disable=canonicalize|cse`.

`test/VC/prune-edges.mlir` @unr needs the rounds: the first round turns
the branch into `^dead` into `assume !c`, the assume-fold rewrite folds
the later branch on `c`, and the next analysis finds every path blocked
(statically safe).

## Exact

Only executions that reach an assert or a halt are observed by the safety
query: an assert fails or holds, a halt is a successful run. An execution
on a dead edge reaches neither: it blocks at the assume the arguments
falsify, or later on every path. After the pass the program has the same
executions that reach an assert or a halt, with the same values and the
same failures; only blocked prefixes are gone. No premise, no entry of the
assumption ledger, no `assume.*` line.

Not for reachability (`sea-vc --reach`): a reach query asks whether a
prefix of an execution reaches a site, also one that blocks afterwards,
and the sites on a dead edge (the falsified assume itself, the blocks
before it) are reached by such prefixes. sea-vc leaves the pass out under
`--reach`, so reach targets and verdicts do not depend on it.

The interpreter on the original program still executes the blocked
prefixes (`assume-blocked` later than the lowered program's run); a
replay of a sat model follows an execution that reaches the assert, which
the pass does not change.

## Counterexample attribution

The pass runs after `sea-merge-asserts`; the branches the merge made of
an assert carry its location wrapped as `loc("sea.assert"(...))` and its
id. The only merged branch the pass can rewrite is one whose continuation
edge is dead: every execution that goes past the assert fails it or
blocks. It becomes `assume !P` and the marked br into the landing block
with the assert id: the replay names that assert, as the original program
does (`--replay-original` match=yes, `test/VC/prune-edges.mlir`
@cont_dead). The landing edge is never dead (the landing block reaches
the exit's assert); if one were, the br would lose the marker and the id,
as an assert that held (SimplifyCondBr's rule, SeaOps.cpp).

## Where it lives, and why there

A pass of its own, right after the final canonicalize + cse of the
lowering (step 7' of `lib/Transforms/Passes.cpp`), before the second
`sea-merge-asserts` and `sea-split-critical-edges`:

- it reads what canonicalize makes: constant block arguments from folded
  values of every lowering (expanded intrinsics, range-fold), and blocks
  that are only `sea.unreachable` (folded assumes of false);
- its rounds need canonicalize; running canonicalize earlier (before the
  merge) would change every program, also the ones with no dead edge.
  Here a program with no dead edge is left exactly as it was: the clean-ups
  run only after a round that pruned;
- not inside `sea-assume-fold`: that pass changes operands only and runs
  before the merge on the program's own asserts; this one changes the CFG
  and needs the clean-ups.

The cost is the analysis per round (linear in edges, a bounded evaluation
per edge into a block with leading assumes) and one canonicalize + cse
per pruning round.

## Counters

On the program as `sea.prune_edges` (only when an edge was pruned),
printed by sea-vc as `vc.prune-edges:` and by the log `lower.prune-edges`
(level 1 per run, level 2 per pruned edge):

- `edges`: dead edges removed (at the first dead edge of each path from
  the entry); of them `by-args` (its own arguments falsify the target's
  assume) and `dead-target` (every path from its target blocks);
- `falsified`: edges whose constant arguments falsify the target's assume,
  among the blocks reachable before each round (summed over rounds). Such
  an edge is often behind a removed one (a br in a block that a cond_br
  enters), so `falsified` can exceed `by-args`;
- `cond-br`, `unreachable`: branches made an assume and a br / made
  `sea.unreachable`;
- `blocks`: blocks reachable before a round (constant branches taking
  their one edge) and not after its rewrite;
- `rounds`: rounds that pruned; `fold-uses`: uses folded by the
  assume-fold rewrite between rounds.

## Open

- The evaluation uses the op folders only. A conjunction with one false
  operand (`and %flag, %x` with `%flag = 0`) is not 0 to it until the
  folders know `x & 0` (identity folders, planned).
- An edge with a non-constant argument whose value a dominating fact
  fixes (an assume of `x == K` before the branch) is not evaluated: the
  integer facts are not substituted yet.
