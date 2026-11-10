# Why trust it

A solver can prove the formula it was given even if a mistake in the
verifier made that formula describe the wrong program. Trust therefore
depends on the path from program to query as well as on the solver.

sea-mlir checks that path in several ways. Every counterexample is
replayed in an interpreter, and can be replayed on the program as it was
before any pass. Reach coverage reports which checks of a program an
execution can reach, so a proof that never reaches its assertion shows
up. Development gates compare independent components, compare lowerings
by what they let executions reach, and run a corpus of small adversarial
programs. Lean proofs establish the soundness of the formalized memory
rules. These checks support confidence in the implementation; they do not
independently certify every `unsat` answer end to end.

This page explains what each check covers, which assumptions remain, and
how the checks were found to be needed.

## Counterexamples are replayed

When the solver finds a violation, `sea-vc` does not report the model
directly. It turns the model into concrete inputs and runs the program in
the interpreter, `sea-run`. The interpreter implements the semantics of
the dialect independently of the encoding into SMT. A counterexample is
reported only if the interpreter reaches the same failing assertion with
the same values. If it does not, `sea-vc` exits with status 2 and says so.

The interpreter is itself checked: every operation is compared against its
definition in SMT-LIB, evaluated by z3, on generated inputs.

## Replay on the original program

The replay above runs the lowered program, the one the VC was made from.
A wrong pass of the lowering changes that program, and the model it
produces still replays on it. `--replay-original` closes this gap: after
a confirmed `sat`, `sea-vc` also runs the input program, before any pass,
on the model's inputs. The run must fail the same assertion. If it does
not, `sea-vc` exits with status 6. When the interpreter cannot run the
original program on those inputs, the result is `match=undetermined`, and
the run says so instead of reporting a match.

This check is off by default. It costs one interpreter run per `sat`.

## Which checks can be reached

A proof is vacuous if the assumptions before an assertion cannot all
hold: the solver then proves the assertion without ever reaching it.
`sea-vc --reach` reports, for every assume and assert of the program,
whether some execution reaches it with every earlier assume holding.
Concrete runs come first: earlier witnesses and fuzz trials of `sea-run`
mark every check they pass, and the solver answers only for the checks no
run reached. Each reachable answer comes with an input that replays to
the check. An assertion reported unreachable is a vacuous proof, or dead
code.

The interpreter used by the fuzzer is the same one that replays
counterexamples, so a fuzz trial is an execution of the model that
`sea-vc` checks, not of a separate build ([Fuzzing](../reference/fuzzing.md)).

## Checking each rewrite with `--seam-check`

Before solving, sea-mlir simplifies the verification condition. The memory
rewriter (SeaM) resolves loads against stores, decides whether two
addresses can be equal, and removes memory operations that cannot affect
the result. Most of the verifier's performance comes from these rewrites,
and so do many of its bugs.

With `--seam-check`, the solver checks every rewrite that fires: its
premises must imply that the term before the rewrite equals the term
after. This check is off by default. An invalid rewrite stops the run
with exit status 5. The SeaM fuzzer can run every generated program with
this check.

## The foundations are proved in Lean

The pointer semantics of SeaM, its translation to bit-vectors, and its
rewrite rules are formalized in Lean, and the rules are proved sound for
all values of their operands. The proofs are about the rules, not about
the C++ code that applies them.

Two things connect the proofs to the code. The `sea-seam-export` tool
prints the terms that the C++ implementation builds, and a conformance
check in Lean confirms that each term, and its translation to
bit-vectors, evaluates to the same value as the interpreter. In addition,
a reviewer independent of its authors has compared the C++ code of the
proved rules with the Lean statements.

The formalization lives in its own repository:
[seam-lean](https://github.com/agurfinkel/seam-lean).

## Random programs, three ways

Fuzzers generate programs and compare independent components on them:

- **VC against interpreter.** For random programs with arithmetic,
  branches and memory, the solvers (z3, Yices, Bitwuzla) must agree; every
  counterexample must replay; and for every proof, random executions of the
  program must not violate the assertion.
- **SeaM against the plain encoding.** The same program is verified with
  and without the memory rewriter, under every access pattern the fuzzer
  supports. The verdicts must agree, and every rewrite is checked.
- **Placement against an abstract interpreter.** The static placement of
  objects in memory must compute the same values as an interpreter that
  keeps objects abstract.

## Validating a change of the lowering

The lowering folds and removes code before the VC is built. A fold that is
wrong can turn a reachable assertion into an unreachable one, and the
verdict becomes a false proof. Two gates compare a lowering with and
without a change:

- **Query diff** (`utils/query-diff.py`) compares the queries of two
  builds job by job. It reports what each comparison proves. Two `unsat`
  queries that differ count as `both-unsat`, which keeps the verdict but
  says nothing more about the change, and the summary line counts those
  jobs separately.
- **Pass validation** (`utils/cover-diff.py`) compares the two lowerings by
  reach coverage. Level 0 requires that every check be reachable on both
  sides or unreachable on both, and that the safety verdicts agree.
  Level 1 also requires that the same inputs reach each check, by an
  equivalence query on the program that ends at that check.

The vcc corpus of SeaHorn's exports is too regular to exercise most folds.
So the gate also runs on `test/QA`, a corpus of small programs: the
positive and edge cases of each fold's tests, and the adversarial programs
reviewers wrote to break it. `just qa-coverage` measures how much of each
validated pass the corpus drives, so a thin corpus is visible.

A planted unsound fold tested the gates themselves. The verdicts and the
replay of the lowered program missed it. Replay on the original program,
both levels of pass validation, and three of the four `test/QA` corpora
caught it.

See [Contributor gates](../agents/gates.md#pass-validation) for the
options and classes.

## Agreement with SeaHorn

sea-mlir is the successor to the bounded model checker in SeaHorn. SeaHorn
exports its programs to sea-mlir, and a differential harness runs SeaHorn's
own regression tests through both engines. A test fails when the verdicts
differ.

Agreement on "safe" can be vacuous: two engines can both report a proof
because both lost the assertion. So agreement is not the only check. The
benchmark suites include mutants, programs with a deliberately injected
bug, each of which must produce a counterexample that replays, and reach
coverage shows which assertions a proof reached.

## Benchmarks with expected results

The benchmark suites are taken from real verification work: the
aws-c-common proofs, Firedancer, Stellar, and NKI kernels, plus crafted
programs. Each program has an expected verdict. The release gates fail on
any change of verdict.

Verdicts alone miss performance regressions that do not change an answer.
The gates also bound the work left for the solver, such as the number of
memory-consistency constraints. For example, the NKI gather good jobs
must resolve aliasing statically and leave zero Ackermann constraints.
A gate fails when that count rises, even if the final verdict stays the
same.

## Protected oracles

The checks above are useful only if nobody weakens them to make a failing
build pass. The oracles, meaning the reference implementations, the
expected verdicts and the checkers, are listed in the test suite and
marked as protected. Changing what an oracle accepts requires a
maintainer's review. Fixing the code so that the oracle passes does not.

## Every verdict states its assumptions

A proof from sea-mlir holds under stated assumptions, and each run reports
the ones it used:

- **In-bounds access.** With the memory rewriter, sea-mlir assumes that
  each memory access stays inside the object its pointer came from. Each
  run reports how many accesses and conditions depend on this;
  `--seam-stats` also counts the rewrites that used it. The lowering's
  removal of memory havocs of fresh objects can rest on it too, and then
  reports `assume.a-inb: drop-fresh-memhavoc=<n>`, also without the
  rewriter. This release does not discharge the assumption; that needs a
  separate memory-safety check of the program.
- **Address 0 is null.** Placement puts the null object at address 0, so
  every VC of a lowered program rests on this, and the run reports it
  (`assume.addr0-is-null`). It is a fact about the target, false on some
  embedded and kernel platforms, and no check discharges it.
- **A memory havoc of null does nothing.** This is an open decision of the
  semantics, not yet part of the formal model. Every run that relies on
  it counts and warns (`assume.memhavoc-null-noop`), and
  `--memhavoc-null=error` makes it a memory-safety error instead.
- **Front-end annotations.** Alignment and pointer-arithmetic flags from
  the compiler are trusted. Each has an optional check that asserts every
  promise used.
- **Bounds.** Loops are unrolled by the front end before the program
  reaches sea-mlir. A memory operation with a symbolic length is expanded
  up to a bound, with an explicit assumption that the run reports.

The design follows the principle of conditional soundness: verify under
explicit assumptions, and discharge them with a separate check, not by
verifying again without them. See
[Known limitations](../reference/limitations.md#assumptions-that-are-not-discharged)
for the full list.

## What is not checked

- The in-bounds assumption is not discharged in this release.
- The Lean proofs cover the memory rewriter and the pointer semantics, not
  the whole tool, and not the C++ code that applies the rules. The rest
  is covered by the testing above.
- Replay on the original program, reach coverage and pass validation run
  on request and in the development gates, not on every `sea-vc` run.
- Pass validation covers the optional folds of the lowering. The passes
  that decide semantics, such as the removal of memory havocs of fresh
  objects, run on both sides and are not validated by it.
- Checks of completeness are new. A rewriter can be sound and still miss
  rewrites it should make; the result is a slower proof, not a wrong one.
  The benchmark bounds and a fuzzer that writes the same address in
  different forms are the first checks for this.

## How the checks were found

Bugs are recorded with the check that found them. A bug that a person
found without a check means a check is missing, and the fix adds it. In
one case a change to the memory rewriter made it miss some equal
addresses. Every verdict stayed correct, but proofs of the NKI kernels
left thousands of unnecessary constraints for the solver. No check
failed; someone noticed the regression while writing a tutorial. The fix
came with the benchmark bounds and the address-form fuzzer described
above.

Building a check has found more than running one. A check needs a precise
statement of what it compares, and writing that statement raised
questions the specification had left open: what a counterexample of the
lowered program says about the original one, what it means to reach a
check, what a memory havoc of null does. Answering them found bugs that
the existing gates had passed. Building replay on the original program
exposed two unsound removals of memory havocs, both older than the
replay. Building reach coverage found that a check was reported reachable
on models that violate the in-bounds assumption. Building pass validation found that
its first version never compared assertion failures. Some of the
questions had no answer yet; they became the named assumptions above, so
each run that depends on one now reports it.
