# Proof controls

An `unsat` says that no run of the program fails its assertion. It does
not say that the program has the runs you meant to check. An assumption
that contradicts the others, a loop that the unroll bound cuts short, or
an inductive cut with an infeasible body can remove those runs, and the
proof then holds vacuously. A proof can also check a property too weak to
catch the bugs it is meant to exclude. Controls are further jobs made from
the same harness. Their expected answers test the proof, not the code.

## Judging an answer

- A `sat` counts only when its replay fails the assertion and agrees with
  the model: `replay: assert-failed` and `validate: ... mismatches=0`
  ([Counterexamples](counterexamples.md)).
- An `unsat` holds under the assumptions the run reports, such as
  `seam.inb: assumed`.
- `unknown`, a timeout or a memory-limit stop never counts, for a proof or
  for a control.

## One failure site per harness

A control that expects `sat` is judged by where the replay fails:
`replay: assert-at` names the condition that failed. A harness that routes
every check through one assertion function keeps that location exact.
With several separate failure calls, the compiler can drop the location
of the one that fails, and `assert-at` then names an earlier operation
([Known limitations](../reference/limitations.md)).

## Vacuity twins

A vacuity twin is the proof's program with the claims off and an
`assert(false)` at the point where every path of interest ends, for
example at the end of the step. The expected answer is `sat` with a
validated replay: some run reaches that point.

- `sat` with a validated replay: the end point is reachable.
- `unsat`: no run reaches it. The proof is vacuous.
- `unknown` or a timeout: inconclusive.

A program with an inductive cut (below) needs a second twin, with the
`assert(false)` at the end of the cut loop body. The first twin can be
`sat` through the loop exit alone, a path on which the body never ran.

## Reach jobs

A reach job asserts the negation of one condition that the proof should
cover, such as "an increment overflows the window". A `sat` with a
validated replay shows a run in which the condition holds. It counts only
if `assert-at` names the reach assertion. If the replay stops at another
check, that check failed, and the job says nothing about the condition.
Turning the other checks off in a reach job avoids that case.

## Mutants and reverts

A mutant is the code under test with a planted bug; a revert undoes a
historical fix. The expected answer is `sat`. A mutant that the proof
cannot catch at the export bound, because it behaves like the original
there, is recorded as expected `unsat`.

A mutant is judged against the obligation that should catch it. Its job
must check that obligation. If the job assumes it, or checks only other
properties, the `unsat` it gives says nothing about the mutant
([an example](../tutorials/txn-parse.md#a-constant-the-proof-explained)).

## Check classes

The checks of a harness fall into classes that can be switched on and off
separately:

- the claims, the properties the proof is about;
- safety checks, such as the code's own internal checks and log-and-abort
  paths;
- the inductive-cut checks, that the invariant holds on entry and after
  one iteration;
- contract checks, the preconditions of a contract that replaces code;
- model checks, that the harness's ghost state agrees with the code's
  state.

A job proves a named subset of the classes. Splitting a proof this way
keeps each query small, and a job may assume a class that another job
checks. A mutant runs against the job that checks the class it breaks.

## Inductive cuts

A loop whose iteration count has no small bound cannot be unrolled. A cut
replaces it by one iteration from an arbitrary state that satisfies an
invariant Inv. For a loop `while (c) body`:

```text
assert Inv        // Inv holds on entry
havoc x1 ... xn   // the variables the loop changes take arbitrary values
assume Inv
if (c) {
  body            // one iteration from any state that satisfies Inv
  assert Inv      // the iteration keeps Inv
  assume false    // the path ends here
}
                  // after the loop: any state with Inv and not c
```

With $s$ the loop state and $T$ one iteration, the cut program checks

$$
\mathit{Inv}(s_0), \qquad
\mathit{Inv}(s) \land c(s) \land T(s, s') \implies \mathit{Inv}(s'),
$$

and continues after the loop from any state with $\mathit{Inv}(s) \land
\lnot c(s)$. The proof then holds for any number of iterations. A
counterexample of a cut program is a run of the cut program: it may start
from a havocked loop state that the original program never reaches. It
shows that the invariant and the code disagree; whether the original
program fails is a separate question.

The same scheme proves a step of a protocol or a data structure: assume
the invariant of an arbitrary state, run one operation, and assert the
claims and the invariant. The base case, that the invariant holds
initially, is a separate check.
