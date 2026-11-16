# Fact folds: sources, regions and the rule table

The facts the lowering uses to simplify a program, where each fact holds,
and the rewrites that may use it. One table for the passes that fold by
facts: `sea-assume-fold` (docs/design/AssumeFold.md),
`sea-prune-edges` (docs/design/PruneEdges.md), `sea-range-fold`
(docs/design/RangeFold.md), the constant folds of canonicalize and
`sea-merge-asserts`. Each of those notes describes its pass; this one
states what they share.

Status: draft. Levels L0 (constant rows), L1, L2 (edge facts, select
form), L3 (integer equalities) and L4 (edge pruning): implemented; their
rows follow the passes' design notes. The identity rows of L0 (K7):
implemented. L5: implemented, off by default.

## Terms

- A *fact* is a pair `v = k`: an SSA value and a constant of its type.
- A *use* is an operand of an op. The values a terminator passes to a
  successor's block arguments are operands of the terminator.
- A fact *holds at an op U* if in every execution that executes U, the
  value of `v` at that moment is `k`.
- An op A *properly dominates* an op U (`mlir::DominanceInfo::
  properlyDominates`): they are in one block and A is before U, or A's
  block dominates U's block and is not U's block. Only blocks reachable
  from the entry count; nothing is claimed about the others.
- An execution *is observed* if it reaches a `sea.assert` or a `sea.halt`.
  The safety query asks about observed executions only: an assert fails or
  holds, a halt is a run that ends well. An execution that blocks at an
  assume or stops at `sea.unreachable` is not observed.
- `sea-vc --reach` asks about prefixes of executions (does a run reach a
  site), so it observes more: every check and block of the program. The
  rows remove checks they make constant and blocks they make dead, so
  under `--reach` the fact folds do not run (vacuity reports must see
  every site); `--reach-fold-facts` runs them anyway, for performance
  work, at the cost of those sites.

A row is *exact* if the program after the rewrite has the same executions,
with the same values, ending the same way and failing the same assert.
Rows of L4 are *exact for the safety query*: the same for observed
executions only.

## Sources and regions

A source makes a fact and gives the region where it holds. A rewrite may
use a fact only at an op in its region. The region conditions are the part
of this note that soundness rests on.

| source | fact | region |
|---|---|---|
| `sea.assume %c` at A | `c = 1` | every op A properly dominates |
| `sea.assert %c` at A | `c = 1` | every op A properly dominates |
| edge of `sea.cond_br %c, ^t, ^f`, `^t` and `^f` distinct | `c = 1` on the true edge, `c = 0` on the false edge | the edge; then by the entry rule below |
| definition `%v = op(%a, ...)` | `v = op(a, ...)` | every op that uses `v` |
| L3: a fact `(icmp eq x, K) = 1` or `(icmp ne x, K) = 0`, `x : iN`, `K` constant | `x = K` | the region of the fact it comes from |

Why each region is right:

- Assume: an execution that executes U passed A (dominance) and went on,
  so `c` was 1 at A. `c` is an SSA value and A is between its definition
  and U, so it is 1 at U.
- Assert: the same. An execution goes past an assert only if it held; a
  failed assert ends the execution. This rests on first-failure semantics,
  not on the assert being proved.
- Not the check's own condition operand: A does not properly dominate
  itself.
- Edge: an execution that takes the true edge evaluated `c` to 1 at the
  branch. A cond_br whose two edges enter one block makes no fact.
- Definition: `v` and its operands are SSA values; the equation holds
  wherever `v` can be used. It is used only in derivations.

Entry rule (L2). A fact holds on entry of a block B if, for every edge
into B whose source is reachable from the entry, one of these is true:
the edge makes the fact; or the fact holds at the end of the edge's source
(a check that properly dominates the source's terminator makes it, or it
holds on entry of a block that dominates the source). Then it holds at
every op in every block B dominates, B included. A block with one such
edge, a cond_br edge, takes what that edge makes; every cond_br target of
the BMC form is of this kind.

Why: an execution in a block B dominates entered B, by one of its edges,
on each of which the fact holds. The value was not defined again since:
its definition dominates B, so an execution that passes the definition
again passes B again before it reaches the op.

What is not in a region, and has been a wrong fold when treated as if it
were:

- an op before the check, or in a block the check's block does not
  dominate;
- an op after the join of a sibling that assumes the value
  (`test/VC/assume-fold.mlir` @sibling: the fold gives a wrong unsat);
- a join entered on one edge that makes the fact and one that does not
  (`test/Transforms/assume-fold.mlir` @join_no_fold).

The lowering is loop-free where these passes run. The arguments above use
only that a definition dominates its uses, so they hold with loops too,
but no pass relies on that: `sea-assume-fold` gives a back edge's source
the checks' facts only, and `sea-prune-edges` leaves a program with a
cycle alone.

## Derivations

From a fact and the definitions, more facts with the same region. Each is
a bit-vector identity over i1 (L1, L2) or `iN` (L3).

| id | from | derives |
|---|---|---|
| D1 | `(and a, b) = 1` | `a = 1`, `b = 1` |
| D2 | `(or a, b) = 0` | `a = 0`, `b = 0` |
| D3 | `(xor a, k) = b`, `(icmp eq a, k) = b`, `(icmp ne a, k) = b`, `a : i1`, `k` constant | the one value of `a` |
| D4 | `a = 0` and a user `w = and a, x` | `w = 0` |
| D5 | `a = 1` and a user `w = or a, x` | `w = 1` |
| D6 | `a = b` and a user `w = xor a, k` / `icmp eq a, k` / `icmp ne a, k` | the value of `w` |
| D7 (L3) | `(icmp eq x, K) = 1`, `(icmp ne x, K) = 0`, `x : iN` | `x = K` |

The derived fact is used at an op in the source's region that uses the
derived value. The value's definition dominates that op (SSA), and so do
the definitions the derivation went through, so every equation of the
derivation holds there.

D7 is for integers only. `sea.ptr.cmp eq` compares addresses, and two
pointers with one address can differ in their object; an equal pointer is
not a substitute.

A source that fixes one value both ways is a contradiction: no execution
reaches its region, and either constant is exact there. The same when two
sources disagree at an op.

## The rule table

Each row: what must hold, what is rewritten, the result, why exact.

### L0: constants (no fact)

| id | shape | result | why exact |
|---|---|---|---|
| K1 | pure op, all operands constant, no nsw / nuw | `sea.const` | `evalPureOp`, the one definition of the op |
| K2 | `assume 1`, `assert 1` | erased | it never blocks / never fails |
| K3 | `assume 0` | canonicalize erases the assume; the block ends in `sea.unreachable` | reaching `sea.unreachable` is an infeasible execution, like `sea.assume 0` (interpreter: assume-blocked; the VC constrains the block not to execute). Only the op a blocked run stops at differs, so reach-site lists may differ; outcomes do not |
| K4 | `assert 0` | a direct failure: `sea.br` into its landing block (merge-asserts), or the assert followed by `sea.halt` (canonicalize); the rest of the block is dead | every execution that reaches it fails it |
| K5 | `cond_br k, ^t, ^f`, `k` constant | `sea.br` to the target it takes | the other edge is never taken |
| K6 | `select k, x, y`, `k` constant | the arm | the op's definition |
| K7 | identities: `x&0`, `x&-1`, `x&x`, `x\|0`, `x\|-1`, `x\|x`, `x^x`, `x^0`, `(x^k)^k`, `x+0`, `x-0`, `x-x`, `x*0`, `x*1`, `x<<0`; `icmp` of a value with itself (every predicate), `icmp ult x, 0` / `uge x, 0` (and with 0 on the left); `addr.cmp p, p`; `addr.cmp` of two constant addresses; an all-constant op with nsw / nuw whose evaluation keeps the promise | the operand, or the value evalPureOp gives | an identity for every `x` (z3, every width); a flagged identity folds only where it can never wrap (`x*1 nsw` at i1 does not); constant addresses compare as the VC does (`ADDR(obj) = base`, pointer equality is address equality) |

K5 on a branch `sea-merge-asserts` made of an assert (location
`loc("sea.assert"(...))`): if the constant is 1 the assert held, and the
new br has no marker and no assert id; if 0 it failed, and the br keeps
both. A marked branch on a path that an execution takes after the assert
held makes a replay name that assert for a later failure.

### L1: i1 facts of checks

| id | needs | rewrites | result |
|---|---|---|---|
| F1 | `v = k` from an assume or assert A, by D1-D6; A properly dominates U; U uses `v` | that operand of U | `sea.const k`, without a location |

Every kind of use: branch and select conditions, connective operands,
values passed to block arguments, conditions of later checks. Exact by the
region: the operand reads the same value in every execution that executes
U. Operands only; K2, K4, K5, K6 then act on the constants.

### L2: i1 facts of edges, select form

| id | needs | rewrites | result |
|---|---|---|---|
| F2 | `v = k` holds on entry of B (entry rule), by D1-D6; U is in a block B dominates; U uses `v` | that operand of U | `sea.const k` |
| N1 | `select c, x, 0`, all i1 | nothing: read as `and c, x` by D1, D2 | (none) |
| N2 | `select c, 1, x`, all i1 | nothing: read as `or c, x` by D1, D2 | (none) |

N1 and N2 use no fact and rewrite nothing: the select and the connective
are one function on i1 (no poison in sea), so the fact collectors read the
select form as the connective, and the program keeps its select. A
rewrite to `and` / `or` was measured and rejected: it made VCs larger,
because `and c, 0` and `and 1, x` have no folders yet while the select
folders do (AssumeFold.md, "Select form").

### L3: integer equalities

| id | needs | rewrites | result |
|---|---|---|---|
| F3 | `x = K` by D7 from a fact of L1 or L2; U in that fact's region; U uses `x` | that operand of U | `sea.const K` |

### L4: dead edges

An edge is *dead* if no execution that takes it is observed:

- by its arguments: the target has a `sea.assume`, before its first
  assert or halt, whose condition is 0 when the edge's constant arguments
  are put in (evaluated by `evalPureOp` over the pure ops of the target
  that compute it);
- by its target: every path from the target stops (a `sea.unreachable`,
  an `assume 0`, a dead edge) before an assert or a halt.

| id | needs | rewrites | result |
|---|---|---|---|
| J1 | `cond_br c, ^t, ^f`, one edge dead | the branch | `sea.assume` of the other edge's condition (`c`, or `xor c, 1`), then `sea.br` on the live edge |
| J2 | a branch with no live edge | the branch | `sea.unreachable` |

Exact for the safety query: an execution on a dead edge blocks before any
assert or halt, so it is not observed; the observed executions are the
same, with the same values and failures. The side condition "before its
first assert or halt" is what makes this sound: with an assert between the
edge and the assume, an execution on the edge can fail that assert, and
pruning the edge would lose the failure.

Not exact for reachability: a prefix that blocks later still reaches the
sites on the dead edge. These rows are off under `sea-vc --reach`.

J1 on a marked branch of a merged assert whose continue edge is dead:
`assume !P`, then the marked br with the assert id into the landing block.
The assume J1 makes is a source of L1 for the ops it dominates, so L4 runs
in rounds with L0-L2.

### L5: ranges (off by default)

| id | needs | rewrites | result |
|---|---|---|---|
| G1 | the range of an icmp, an i1 `and` / `or` / `xor`, or a select's condition, at the op P, is one value; the range uses bounds of assumes that properly dominate P | P (the icmp or i1 op), or the select | `sea.const`, or the arm |

The fold is at the definition P, for all its uses, so the assumes must
dominate P, not a use. The range walk reads nsw / nuw flags, which are
trusted: G1 is exact given the flags, and can be stronger than the VC's
encoding of the same ops (docs/design/RangeFold.md).

## Fully simplified at level L

A program is fully simplified at level L if no row of levels 0 to L
applies at any op of a block reachable from the entry. For L1 that reads:
no constant check or branch, and no operand of a non-constant value that a
properly dominating check fixes through D1-D6.

It is a property of a program, checked without running a pass: collect
the facts, test every use. The postconditions of `sea-assume-fold`,
and `sea-prune-edges` each check their own rows on
their own output. The lowering's output should be fully simplified at the
level its switches select; a later pass that makes a new constant or a new
assume (J1) can break an earlier level, which is why L4 runs rounds.

Fully simplified at level L does not say that no simplification is left.
Known cases outside every row, each listed in its pass's note:

- the cond_br's own edge operands (`cond_br %c, ^a(%c), ^b`): the fact
  holds on the edge, the operand is not in a block the target dominates;
- a join whose edges fix a value by checks only (each sibling assumes it):
  the entry rule takes its candidates from cond_br edges;
- `select c, 0, x` and `select c, x, 1` (they need a negation);
- an edge whose non-constant argument a dominating fact fixes (L4 after
  L3);
- an assume that post-dominates a branch on its value: below.

## Not in the table: facts from later assumes

Every source above gives a fact about what is true *after* it. An assume
also says something about the executions *before* it: the ones that reach
it with its condition 0 block there. It is tempting to use `c = 1` at an
op the assume post-dominates. That is not a fact in the sense of this
note: `c` can be 0 at that op in an execution, and the execution is real
up to the assume. A fold of `c` to 1 there is wrong whenever something
between the op and the assume observes the execution:

- an assert between them can fail with `c = 0`; after the fold that
  failure is gone, or the assert that fails first changes;
- a halt between them ends a run that the fold has changed;
- post-dominance computed to the exit does not see either: it must be
  taken to the first assert or halt, per path.

The one use of this reasoning the table admits is L4, in a form that
changes control only and carries its side condition: an edge is removed
when every execution on it blocks before any assert or halt. It never
gives a value at an earlier op. A branch that such an assume makes
one-sided is folded by J1 turning the branch into an assume, which is then
an ordinary source of L1 for what it dominates. A general row for
post-dominating assumes would need a second region notion (holds for the
executions that are observed, at the ops from which no assert or halt is
reached before the assume) and its own argument for failure attribution.
Until that is written and checked, `vc.assumed-branches` reports such
branches and no pass folds them.
