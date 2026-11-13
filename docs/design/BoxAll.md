# Claims over an index box: sea.box.all

`sea.box.all` (ODS: `include/sea/Dialect/Sea/SeaOps.td`; parser, printer,
verifier: `lib/Dialect/Sea/SeaOps.cpp`), its interpreter evaluation
(`lib/Interp/Interpreter.cpp`, `execBoxAll`), the trip count
(`boxTripCount`, `include/sea/Dialect/Sea/Semantics.h`) and its VC lowering
`sea-lower-box-all` (`lib/Transforms/LowerBoxAll.cpp`, the first pass of
`sea-vc --lower` after `sea-apply-tracking`). Tests:
`test/Dialect/Sea/box-all.mlir`, `test/Dialect/Sea/box-all-invalid.mlir`,
`test/Interp/box-all.mlir`, `test/Transforms/lower-box-all.mlir`,
`test/Transforms/lower-box-all-errors.mlir`, `test/VC/box-all-bounds.mlir`,
`test/VC/box-all-equal-regions.mlir`.

Status: implemented (v1). Not a general quantifier.

## Why

Harnesses often claim that a property holds at every index of a range: two
memory regions are equal, a frame of a buffer is unchanged, every row
satisfies an invariant. Written as a conjunction over all cells, each
conjunct is a read of the final memory through every write: for a range of
n cells and w writes, the query holds n read chains of length w. A front
end can instead pick one nondet index, bound it, and assert the property at
that index: the claim holds at every index iff it holds at an arbitrary
one (in assert position). When each instance is expensive, the one-index
form can be much faster.

Writing that by hand is easy to get subtly wrong: the bound, the empty
range, the step, a claim that is also used as a value or under a negation
(where one arbitrary index is no longer the same claim). `sea.box.all`
states the claim over the whole range, so the interpreter checks the full
claim, and one lowering produces the one-index form, the same way every
time, only where it is exact.

## The op

```mlir
%eq = sea.box.all (%i) = (0) to (%n) {
  %pa = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %pb = sea.ptr.add %b, %i : !sea.ptr<64>, i64
  %x = sea.load %m[%pa] : !m -> i8
  %y = sea.load %m[%pb] : !m -> i8
  %e = sea.icmp eq, %x, %y : i8
  sea.yield %e
}
sea.assert %eq

%ok = sea.box.all (%r, %c) = (0, 1) to (%rows, %cols) step (1, 2) { ... }
```

- 1 or 2 dimensions (`kMaxDims`). One covers ranges of memory; two cover
  rows x columns. The lowering and the interpreter are written for any
  number; the limit keeps the tested surface small. Raising it is a
  constant and tests.
- Per dimension: a lower bound `lo` (a constant in [0, 2^63)), a step (a
  constant in [1, 2^63)), an upper bound `ub` (an i64 SSA value; a
  `sea.const` for a constant). The body is one block whose arguments are
  the indices (i64), ending with `sea.yield %v : i1`.
- The result is i1.

### Semantics

Dimension d ranges over the indices k = lo + step * t for
t = 0, 1, ..., N - 1, where the trip count is

    N = 0                              if ub <=u lo
    N = (ub - lo - 1) /u step + 1      otherwise

Unsigned, the upper bound exclusive, no wrap (every index is < ub). The box
is the product of the dimensions. The result is 1 iff the body yields 1 at
every index of the box; an empty box (some N = 0) gives 1.

Unsigned rather than signed: the upper bounds are sizes and counts, and a
size with the top bit set must not make the box empty (a signed reading
would, and the claim would hold vacuously). Lower bounds are non-negative
constants, so signed and unsigned agree whenever ub < 2^63.

### The body

The verifier admits, in the body, sea ops without regions, successors or
effects (`isMemoryEffectFree`) and without memory results: value ops,
address and pointer arithmetic and comparisons, struct ops, `sea.load` and
`sea.mem.is_modified` of a memory defined outside the op. It refuses (one
error naming the reason): stores, memset / memmove / memhavoc, nondets,
assume / assert, allocations, prints, nested regions (a nested box too),
control flow, ops of other dialects, and the ops that name an object
(`sea.segment`, `sea.global`, `sea.func_addr`: define the object outside).
Values defined outside the op may be used in the body.

### Form chosen, and the upstream forms

`affine.parallel` with `reduce ("andi")` and `scf.parallel` with an
`scf.reduce` are the closest upstream forms; `scf.forall` has no scalar
reduction. Reusing one directly was rejected:

- their induction variables and bounds are `index`; sea values are
  bit-vectors, i64 here (every bound and index would need casts, and the
  VC has no `index` sort);
- `affine.parallel` bounds are affine maps of dims and symbols, with
  restrictions on what may be a symbol; our upper bounds are arbitrary i64
  values;
- `scf.parallel`'s reduction is a general region and `affine.parallel`
  names any of several reductions: the op could not say that only the
  conjunction is meant;
- an upstream op's verifier cannot restrict its body to the pure sea ops
  the interpreter and the lowering handle.

`sea.box.all` is a sea op that mirrors their syntax and structure
(`(%i, %j) = (lo...) to (ub...) step (s...)`, a single-block region with
the induction variables as arguments, a yield) with the restrictions in
the types and the verifier. The name says what it is: a box of indices
and the conjunction ("all") over it; not `forall`.

## Interpreter

The interpreter evaluates the body at every index of the box, row-major
(the last dimension fastest), and defines the result as their conjunction.
It evaluates the whole box (no short cut at a false index), so a load out
of its object at any index of the box is reported as it would be for the
same loads written out. Each body op counts one step: a box too large to
run ends at the step limit (status `step-limit`). The run records the first
false evaluation of a box op and its first false index: sea-run prints
`box-all-false: index=(<k>[, <k>]) op=<where>` with a failing assert
(jsonl: `box_all_false` in the status record), and `sea-vc
--replay-original` adds `box-all-false=(...)` to `replay.original:`.
With an SSA upper bound, `--replay-original` is usually
`match=undetermined`: the model is free to pick a huge bound, and the run
of the original reaches the step limit; the replay of the lowered program
still validates the counterexample.

`Semantics.cpp` has the trip count (`boxTripCount`), used by the
interpreter and by the lowering for constant bounds. The op is not in
`evalPureOp` and not in `utils/check-semantics.py`: its value is not a
function of its operand values alone (it evaluates a region), and the
oracle checks bit-vector operations against z3 case by case; the body's
ops are already in it. The op's own semantics (trip count, emptiness,
conjunction, failing index) is pinned by `test/Interp/box-all.mlir` and,
against the lowering, by `test/VC/box-all-bounds.mlir`.

## VC lowering: sea-lower-box-all

### Assert polarity (the rule)

A box op is lowered only if every use of its result is

- the condition of a `sea.assert`, or
- an operand of an i1 `sea.and` / `sea.or` whose result is, recursively,
  in assert polarity.

The program's assertion is then a monotone function F of the claim A. With
B(t) the body at the index of t, A <= B(t) for every t (A is their
conjunction) and A = B(t0) for some t0 (a false index, if any). So F(A) is
0 iff F(B(t)) is 0 for some t: the lowered program fails on some input iff
the original does. This holds for each box op independently, so several
in one assertion, or one feeding several asserts, are fine.

The rule follows the uses of the claim only. The gated form
`or(not ok, all)` is in scope: the negation is of `ok`, not of the claim;
so is the product shape `and(ok_l == ok_r, or(ok_l == 0, all))`.

Refused, one error per op (kind `box-all.<reason>`), and then the lowering
stops (no partial lowering, no unrolling fallback):

| kind | the claim is used by |
|---|---|
| `box-all.use-assume` | `sea.assume` (one index would be a weaker assumption) |
| `box-all.use-branch` | `sea.cond_br` / `sea.br` |
| `box-all.use-negation` | `sea.xor`, `sea.icmp` (e.g. `icmp eq all, 0`) |
| `box-all.use-select` | `sea.select` |
| `box-all.use-print` | a strong `sea.print` (it would show one index's value) |
| `box-all.use-value` | anything else (a cast, arithmetic, a store) |

A box op without uses is erased (its body is pure).

### The replacement

Per dimension d, at the op's place:

    t_d = sea.nondet "box.all.<tag>.<d>" [<fresh site>] : i64
    sea.assume (sea.icmp ult, t_d, N_d)
    k_d = lo_d + step_d * t_d              (omitted parts: step 1, lo 0)

then the body, cloned with k_d for its arguments; the yielded value
replaces the result. `<tag>` is the op's `NameLoc` name, else `b<n>`, its
position among the program's box ops; the site is fresh (above every site
of the program); the nondet's location is a `NameLoc` of its callee. The
nondet is the trip number t, an ordinary input: a sat model gives it, the
replay reads it, and with lo = 0, step = 1 it is the index itself.

The assume is one unsigned compare of the nondet with the trip count (the
form the fuzzer and `sea-range-fold` read). It must never block a path the
original takes; three cases, by what is known statically
(`foldConstant` of the upper bounds):

1. Every N_d a constant >= 1: the replacement above, straight-line.
2. Some N_d a constant 0: the box is empty, the result is `true`.
3. Otherwise the box may be empty: the op's block is split at the op,
   `sea.cond_br (and_d ub_d >u lo_d), ^box, ^join(true)`, the nondets,
   assumes and body in `^box` with N_d = (ub_d - lo_d - 1) /u step_d + 1
   (ub_d - lo_d for step 1), which ends `sea.br ^join(v)`; the claim is
   `^join`'s argument. Block ids as `sea-guard-null-memhavoc`.

Why the guard in case 3: with `assume(t ult N)` and N = 0, the path would
be blocked, and with it every other check of the assertion
(`test/VC/box-all-bounds.mlir`, `@empty_keeps_path`). Clamping N to 1 and
or-ing the emptiness would keep the path but evaluate the body at an
index outside the box: a load the original program never makes, which the
replay and a bounds-check job would report.

No-wrap flags (`nsw` / `nuw`) of body ops become flags of the cloned ops,
so `sea-wrap-flags` turns them into an assume or assert at the chosen
index only. `mode=assert` is exact (the promise asserted at an arbitrary
index of the box, in assert polarity). `mode=assume` is weaker than the
promise at every index: executions that break it at another index stay;
that is sound, it can only add failures.

Placement in the lowering: first after `sea-apply-tracking`, before
`sccp`. sccp cannot see into the region, and no later pass is written for
region ops; the polarity test reads the program as the front end wrote it.

### Expansion of small boxes

`sea-vc --box-all-expand=K` (pass option `expand-max`; K <= 65536, else an
error): a box whose upper bounds are all constants and whose trip count (the product over its
dimensions) is at most K is replaced by the body at every index,
row-major, and-reduced (`and(and(b0, b1), b2)`...). The front end states
the claim; the lowering picks the form. Expansion is exact in any
position, but it is taken only under the same polarity rule and refusals
as the one-index form: what a front end may write must not depend on K.
Nothing else expands: no symbolic upper bound (even one assumed small), no
partial unrolling. K = 0 turns it off.

Constant means: the op folders reduce the bound to one value
(`foldConstant`): a `sea.const`, or flag-free arithmetic on constants. The
pass runs before sccp, so a block argument (even one every predecessor
passes the same constant) and arithmetic with `nsw` / `nuw` (ops with
flags do not fold) are not constant here: such a box gets the one-index
lowering.

Default K = 16, untuned. Measured on the program of
`test/VC/box-all-equal-regions.mlir` (`@box`, generated at n cells: a
forward and a backward copy of n bytes, claim equal; `--lower --seam`, z3,
solve time in seconds of a first run; n = 32, 48, 64 were run again, in
parentheses: that run's solve time / wall time of the whole sea-vc run;
n = 48 only then):

| n | one index: solve | Ackermann constraints | expanded: solve | Ackermann constraints |
|---|---|---|---|---|
| 4 | 0.081 | 9 | 0.033 | 0 |
| 8 | 0.142 | 17 | 0.057 | 0 |
| 12 | 0.230 | 25 | 0.092 | 0 |
| 16 | 0.349 | 33 | 0.171 | 0 |
| 32 | 0.680 (0.709 / 0.85) | 65 | 0.597 (0.597 / 0.89) | 0 |
| 48 | (1.067 / 1.27) | not taken | (1.040 / 1.60) | not taken |
| 64 | 1.225 (1.138 / 1.42) | 129 | 2.401 (1.794 / 2.56) | 0 |

Expanded, SeaM decides every read (constant offsets from the same symbolic
bases); one index leaves the reads at `t` against every store. Expansion
wins up to about 32 cells here and loses beyond (the query grows with n
read chains of 2n stores, and query building with it); 16 is well inside
the winning side. One program and one solver: the default is a work
item, not a tuned value.

### Counters

On the program (`sea.box_all`) and in `sea-vc`'s output:
`vc.box-all: dead=<n> empty=<n> expand-max=<K> expanded=<n> guarded=<n>
lowered=<n> nondets=<n>` (lowered: one index, guarded included; expanded
boxes are not in lowered). The log tag `lower.box-all` (level 1: the
counters, with `refused-use-<reason>=<n>` on refusal; level 2: each op).

`sea-vc` without `--lower` refuses a program with a box op
(`vc.box-all-unlowered`).

## When to use it

Skolemizing a universal claim is not always faster:

- claims whose instances are each expensive (an equality of two memories,
  a frame condition: each instance is a read through all writes): the
  one-index form can be much faster than all cells;
- a pairwise-uniqueness claim over rows (`dest_p != dest_q` for p < q),
  where the contract assumes pairwise distinctness instance by instance:
  the one-nondet-pair form was slower at every size measured. Hypothesis:
  each explicit instance is
  discharged by its matching assumption instance; one arbitrary instance
  hides both behind select chains and the solver case-splits.

Rule: use `sea.box.all` for claims whose instances are each expensive;
keep claims whose instances are matched one to one by assumption
instances as explicit conjunctions. (A p < q triangle is not a box
anyway.) The test `test/VC/box-all-equal-regions.mlir` shows the query
shape (fewer read chains) on a small case, not a speedup: at 12 cells SeaM
decides the cell-by-cell form statically and it is the faster one (which
is why small constant boxes are expanded by default, see above).

## Open questions (not in v1)

- An `exists` dual (a disjunction over a box: one nondet index in assume
  polarity, or in negated-assert polarity).
- Expansion beyond constant boxes of at most K indices (symbolic bounds
  with a known small range, partial unrolling), or a K per claim, e.g. for
  claims like the pairwise case above; a tuned default K.
- Passing the quantifier to the solver (`forall` over bit-vectors).
- Non-box iteration spaces (triangles p < q), more than 2 dimensions,
  index widths other than i64, signed bounds, negative lower bounds.
- A `sea.print` of the index (lo + step * t) in the lowered program, so a
  replay shows the index rather than the trip number when lo != 0 or
  step != 1.
- Passes other than the VC lowering (placement, the fat-pointer and
  tracking lowerings run alone) are not written for region ops; run them
  after `sea-lower-box-all`.
