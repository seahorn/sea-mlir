# Why trust it

A verifier is trusted for its answers, and an answer is only as good as
the path from the program to the query the solver sees. sea-mlir treats
checking that path as part of the work, not as a step after it. Every
part of the pipeline comes with a check that can show it wrong, built
together with the part.

Building a check has found more bugs than running one. A check must state
exactly what it compares, and writing that statement raises questions the
specification left open. Answering them has found bugs that every existing
test passed. A question that has no answer yet becomes a named
assumption, and every run that depends on it says so.

What this means for an answer from `sea-vc`:

- **A counterexample is real.** It is replayed in an independent
  interpreter before it is reported.
- **A proof holds under the assumptions the run reports.** Each run names
  the assumptions it used and counts how much of the proof depends on
  each.
- **The limits are stated.** This page says which parts are proved, which
  are tested, and [what is not checked](#what-is-not-checked).

The rest of the page goes deeper, one level at a time: what an answer
means, the checks behind it, and how the checks themselves were tested.

## What an answer means

### Counterexamples are replayed

When the solver finds a violation, `sea-vc` does not report the model
directly. It turns the model into concrete inputs and runs the program in
the interpreter, `sea-run`. The interpreter implements the semantics of
the dialect independently of the encoding into SMT. A counterexample is
reported only if the interpreter reaches the same failing assertion with
the same values. If it does not, `sea-vc` exits with status 2 and says so.

### Proofs state their assumptions

A proof from sea-mlir holds under explicit assumptions, and each run
reports the ones it used:

- **In-bounds access.** With the memory rewriter, sea-mlir assumes that
  each memory access stays inside the object its pointer came from. Each
  run reports how many accesses and conditions depend on this.
- **Address 0 is null.** Every VC of a placed program rests on this. It is
  a fact about the target platform, false on some embedded and kernel
  platforms.
- **A memory havoc of null does nothing.** This is an open decision of the
  semantics. Every run that relies on it counts and warns.
- **Front-end annotations.** Alignment and pointer-arithmetic flags from
  the compiler are trusted. Each has an optional check that asserts every
  promise used.
- **Bounds.** Loops are unrolled by the front end. A memory operation with
  a symbolic length is expanded up to a bound, which the run reports.

The design follows the principle of conditional soundness: verify under
explicit assumptions, and discharge them with a separate check, not by
verifying again without them.
[Known limitations](../reference/limitations.md#assumptions-that-are-not-discharged)
lists each assumption with its counters and options.

### Proofs can show what they reached

A proof is vacuous if the assumptions before an assertion cannot all hold:
the solver then proves the assertion without any execution reaching it.
`sea-vc --reach` reports, for every assume and assert of the program,
whether some execution reaches it with every earlier assume holding, and
gives an input that does. An assertion reported unreachable is a vacuous
proof or dead code.

## The checks

### Checks a user can run

- **Replay on the original program** (`--replay-original`). The ordinary
  replay runs the simplified program the VC was built from, so a wrong
  simplification produces a counterexample that still replays. This check
  also runs the input program, before any pass, on the model's inputs. It
  must fail the same assertion; otherwise `sea-vc` exits with status 6.
- **Reach coverage** (`--reach`). Concrete runs come first: earlier
  witnesses and fuzz trials mark every check they pass, and the solver
  answers only for the checks no run reached.
- **Rewrite checking** (`--seam-check`). The memory rewriter (SeaM)
  resolves loads against stores, decides whether two addresses can be
  equal, and removes memory operations that cannot affect the result. With
  this option, the solver checks every rewrite that fires: its premises
  must imply that the term before the rewrite equals the term after. An
  invalid rewrite stops the run with status 5.
- **Fuzzing the model** (`sea-run --fuzz`). The fuzzer runs the same
  program, in the same interpreter, that `sea-vc` checks, so what it finds
  is a fact about the model and not about a separate build
  ([Fuzzing](../reference/fuzzing.md)).

The first three are options of `sea-vc`, off by default.

### Proofs in Lean

The pointer semantics of SeaM, its translation to bit-vectors, and its
rewrite rules are formalized in Lean, and the rules are proved sound for
all values of their operands. The proofs are about the rules, not about
the C++ code that applies them.

Two things connect the proofs to the code. The `sea-seam-export` tool
prints the terms that the C++ implementation builds, and a conformance
check in Lean confirms that each term, and its translation to
bit-vectors, evaluates to the same value as the interpreter. In addition,
a reviewer independent of its authors has compared the C++ code of the
proved rules with the Lean statements. The formalization lives in its own
repository: [seam-lean](https://github.com/agurfinkel/seam-lean).

### Gates on every change

Development gates run before a change is merged:

- **Interpreter against SMT-LIB.** Every operation of the interpreter is
  compared with its definition in SMT-LIB, evaluated by z3, on generated
  inputs.
- **Random programs.** Fuzzers generate programs and compare independent
  components on them. The solvers (z3, Yices, Bitwuzla) must agree, every
  counterexample must replay, and random executions must not violate a
  proved assertion. The same program is verified with and without the
  memory rewriter, and the verdicts must agree. Static placement of
  objects must compute the same values as an interpreter that keeps
  objects abstract.
- **Agreement with SeaHorn.** sea-mlir is the successor to the bounded
  model checker in SeaHorn. A differential harness runs SeaHorn's
  regression tests through both engines, and a test fails when the
  verdicts differ.
- **Benchmarks with expected results.** The suites come from real
  verification work: the aws-c-common proofs, Firedancer, Stellar, and NKI
  kernels, plus crafted programs. A change of any verdict fails the gate.
  The gates also bound the work left for the solver, such as the number of
  memory-consistency constraints, so a change that makes proofs harder
  fails even when every verdict stays the same. Mutants, programs with a
  deliberately injected bug, must each produce a counterexample that
  replays.
- **Query diff.** A change that must not change the queries, such as a
  refactoring, runs `utils/query-diff.py` over a corpus. It reports what
  each comparison proves; two `unsat` queries that differ count as
  `both-unsat`, which keeps the verdict and proves nothing more.
- **Pass validation.** A change of the simplification before the VC runs
  `utils/cover-diff.py`, which compares the two versions by reach coverage.
  Level 0 requires that every check be reachable in both or in neither,
  and that the verdicts agree. Level 1 also requires that the same inputs
  reach each check.

### Protected oracles

The checks above are useful only if nobody weakens them to make a failing
change pass. The oracles, meaning the reference implementations, the
expected verdicts and the checkers, are listed in the test suite and
marked as protected. Changing what an oracle accepts requires a
maintainer's review. Fixing the code so that the oracle passes does not.

## What is not checked

- The in-bounds assumption is not discharged in this release; that needs
  a separate memory-safety check of the program. Neither is the
  assumption that address 0 is null, which no check of the program can
  discharge.
- The Lean proofs cover the memory rewriter and the pointer semantics, not
  the whole tool, and not the C++ code that applies the rules.
- Replay on the original program, reach coverage and rewrite checking run
  on request and in the development gates, not on every `sea-vc` run.
- Pass validation covers the optional simplifications. The passes that
  decide semantics, such as the removal of memory havocs of fresh
  objects, are not validated by it.
- Checks of completeness are new. A rewriter can be sound and still miss
  rewrites it should make; the result is a slower proof, not a wrong one.
  The benchmark bounds and a fuzzer that writes the same address in
  different forms are the first checks for this.

## How the checks were tested and found

### Testing the gates

A gate that passes everything proves nothing, so the gates are tested
with planted bugs. One was a fold that forwards a load across a store to a
different address. The verdicts on the vcc corpus missed it, and so did
the replay of the simplified program. Replay on the original program
caught it, level 0 of pass validation caught it on 48 vcc jobs, level 1
caught it on jobs level 0 missed, and three of the four `test/QA` corpora
caught it.

The vcc corpus of SeaHorn's exports turned out too regular for this work:
it contains none of the programs that broke the simplifications. So pass
validation also runs on `test/QA`, a corpus of small programs: the
positive and edge cases of each fold's tests, and the adversarial
programs reviewers wrote to break it. `just qa-coverage` measures how much
of each validated pass the corpus drives, so a thin corpus is visible.
[Contributor gates](../agents/gates.md#pass-validation) gives the options
and classes.

### Bugs found by building checks

Bugs are recorded with the check that found them. Some examples:

- Building replay on the original program exposed two unsound removals of
  memory havocs, both older than the replay: one treated a constant
  memory as fresh, the other walked past writes that the allocation did
  not dominate. Both turned a reachable violation into a proof.
- Building reach coverage found a check reported reachable on models that
  violate the in-bounds assumption.
- The first version of pass validation never compared assertion
  failures. Its own review found this.
- Running the fuzzer on SeaHorn's exports found that the interpreter and
  the VC disagreed on a memory havoc of a null pointer. The decision is
  now explicit (`--memhavoc-null`), and runs that depend on it report it.

A bug that a person found without a check means a check is missing, and
the fix adds it. In one case a change to the memory rewriter made it miss
some equal addresses. Every verdict stayed correct, but proofs of the NKI
kernels left thousands of unnecessary constraints for the solver. No check
failed; someone noticed the regression while writing a tutorial. The fix
came with the benchmark bounds and the address-form fuzzer described
above.
