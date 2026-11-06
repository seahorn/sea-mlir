# Why trust it

A verifier can be wrong in two directions. A wrong counterexample wastes
time and is easy to spot. A wrong proof is dangerous: it reports that a
property holds when it does not, and nothing in the output shows it.
sea-mlir is built so that each of its answers is checked by something
other than the code that produced it.

This page describes those checks, what each one covers, and what is not
checked.

## Counterexamples are replayed

When the solver finds a violation, `sea-vc` does not report the model
directly. It turns the model into concrete inputs and runs the program in
the interpreter, `sea-run`. The interpreter implements the semantics of
the dialect independently of the encoding into SMT. A counterexample is
reported only if the interpreter reaches the same failing assertion with
the same values. If it does not, `sea-vc` exits with status 2 and says so.

The interpreter is itself checked: every operation is compared against its
definition in SMT-LIB, evaluated by z3, on generated inputs.

## Every rewrite is checked

Before solving, sea-mlir simplifies the verification condition. The memory
rewriter (SeaM) resolves loads against stores, decides whether two
addresses can be equal, and removes memory operations that cannot affect
the result. These rewrites are where a verifier's performance comes from,
and also where its bugs tend to be.

With `--seam-check`, every rewrite that fires is checked by the solver: its
premises must imply that the term before the rewrite equals the term
after. An invalid rewrite stops the run with exit status 5. The SeaM
fuzzer can run every generated program with this check.

## The foundations are proved in Lean

The pointer semantics of SeaM, its translation to bit-vectors, and its
rewrite rules are formalized in Lean, and the rules are proved sound for
all values of their operands. The proofs are about the rules, not about
the C++ code that applies them.

Two things connect the proofs to the code. The `sea-seam-export` tool
prints the terms that the C++ implementation builds, and a conformance
check in Lean confirms that each term, and its translation to
bit-vectors, evaluates to the same value as the interpreter. In addition,
the C++ code of the proved rules has been reviewed against the Lean
statements by a reviewer independent of its authors.

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

## Agreement with SeaHorn

sea-mlir is the successor to the bounded model checker in SeaHorn. SeaHorn
exports its programs to sea-mlir, and a differential harness runs SeaHorn's
own regression tests through both engines. Any test where the verdicts
differ is a failure, not a tolerance.

Agreement on "safe" can be vacuous: two engines can both report a proof
because both lost the assertion. So agreement is not the only check. The
benchmark suites include mutants, programs with a deliberately injected
bug, each of which must produce a counterexample that replays.

## Benchmarks with expected results

The benchmark suites are taken from real verification work: the
aws-c-common proofs, Firedancer, Stellar, and NKI kernels, plus crafted
programs. Each program has an expected verdict. The release gates fail on
any change of verdict.

Verdicts alone miss performance regressions that do not change an answer.
The gates also bound the work left for the solver, such as the number of
memory constraints. For example, the NKI gather proofs must leave no
memory constraints at all, and a gate fails when one does.

## Protected oracles

The checks above are only useful if nobody weakens them to make a failing
build pass. The oracles, meaning the reference implementations, the
expected verdicts and the checkers, are listed in the test suite and
marked as protected. Changing what an oracle accepts requires a
maintainer's review. Fixing the code so that the oracle passes does not.

## Every verdict states its assumptions

A proof from sea-mlir holds under stated assumptions, and each run reports
them:

- **In-bounds access.** With the memory rewriter, sea-mlir assumes that
  each memory access stays inside the object its pointer came from. Each
  run reports how many accesses and conditions depend on this;
  `--seam-stats` also counts the rewrites that used it. This release does not discharge the assumption;
  that needs a separate memory-safety check of the program.
- **Front-end annotations.** Alignment and pointer-arithmetic flags from
  the compiler are trusted. Each has an optional check that asserts every
  promise used.
- **Bounds.** Loops are unrolled by the front end before the program
  reaches sea-mlir. A memory operation with a symbolic length is expanded
  up to a bound, with an explicit assumption that the run reports.

The design follows the principle of conditional soundness: verify under
explicit assumptions, and discharge them with a separate check, not by
verifying again without them.

## What is not checked

- The in-bounds assumption is not discharged in this release.
- The Lean proofs cover the memory rewriter and the pointer semantics, not
  the whole tool, and not the C++ code that applies the rules. The rest
  is covered by the testing above.
- Checks of completeness are new. A rewriter can be sound and still miss
  rewrites it should make; the result is a slower proof, not a wrong one.
  The benchmark bounds and a fuzzer that rewrites the same address in
  different forms are the first checks for this.
- There is no per-assertion vacuity check. Mutants in the benchmark
  suites cover vacuity for the benchmarks only.

## When a check finds a bug

Bugs are recorded with the check that found them. A bug found by a person
rather than by a check means a check is missing, and the fix includes the
missing check. One example: a change to the memory rewriter made it miss
some equal addresses. Every verdict stayed correct, but proofs of the NKI
kernels left thousands of unnecessary constraints for the solver. No check
failed; the regression was noticed while writing a tutorial. The fix came
with the benchmark bounds and the address-form fuzzer described above.
