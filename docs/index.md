---
template: home.html
title: sea-mlir
hide:
  - navigation
  - toc
---

<p class="lead">sea-mlir is a verification toolkit for low-level code, built for AI agents as well as people. Programs are written in a small MLIR dialect: control flow, bit-vector arithmetic, and byte-addressed memory. The toolkit checks assertions in those programs with SMT solvers and returns either a proof or a concrete counterexample that the interpreter replays.</p>

## Assembly for verification

Automated verification often stalls on encoding, not on the program. When
a proof is slow or fails, someone has to find what makes the problem hard
and change how it is posed: the memory model, the decomposition, the
assumptions, the solver settings.

Neither source code nor SMT is a good place to do that. Source code keeps
the programmer's intent, but a compiler front end stands between a source
edit and its effect on the formula; a small change in the source can make
a large, unpredictable change in the encoding. SMT shows the constraints,
but after expansion (Ackermann constraints for memory, bit-blasting
for arithmetic) the program's structure is gone.

sea-mlir sits between the two. Its programs keep control flow, arithmetic
and memory operations, stay close to the program in size, and stay close
to the solver encoding. The encoding choices are explicit options of the
tools, so an agent can change one, rerun, and compare.

## Built for agents

An agent working on a hard proof inspects the program, chooses what to
process, adjusts parameters, and reads the result. sea-mlir is designed for
that loop:

- every tool has an `--agent` guide and machine-readable output (`--format=jsonl`);
- exit codes are stable and documented;
- every counterexample is replayed by an interpreter before it is reported;
- statistics name the operations and rules responsible for the solver's work;
- every result states the assumptions it depends on.

The configuration an agent arrives at is saved as a profile, and CI runs
it on later revisions of the code. Revisions of the same code usually
verify with the same profile; the agent returns only when a revision does
not.

## How it differs

- **Proof assistants (Lean, Rocq, Isabelle).** A proof assistant checks a
  proof of one statement; when the code changes, the proof has to be
  repaired. In sea-mlir the agent's work product is a decision procedure
  specialized to the application: the encoding, the rewriting, the
  decomposition and the solver settings. CI then proves each new revision
  with it automatically, and the solver does the bulk of the work. We use
  Lean, with agents writing the proofs, for the foundations of sea-mlir's
  rewriting.
- **Intermediate verification languages (Boogie, Why3, Viper).** They offer
  high-level types, quantifiers, procedure contracts, and loop invariants
  for unbounded proofs. sea-mlir works at the machine level: bit-precise
  arithmetic, byte-addressed memory, and concrete executions as
  counterexamples.
- **Compiler IRs (LLVM IR).** LLVM IR is designed for optimization, and its
  semantics include undefined behavior. sea-mlir has one executable
  semantics shared by the interpreter and the verifier.
- **Solver formats (SMT-LIB).** SMT-LIB describes formulas, not programs.
  sea-mlir produces SMT-LIB and keeps the program it came from.

## Why trust the results

A verifier is only useful if its answers are right. sea-mlir checks itself
in several independent ways: counterexample replay, a solver check of every
rewrite it applies, differential runs against SeaHorn, fuzzing with
mutants, and machine-checked proofs in Lean of the rewriting foundations.
[Quality assurance](trust/index.md) describes each check and what it
covers.

## Start here

- [Get started](guide/getting-started.md): build, run a first proof, read the output.
- [Applications](tutorials/index.md): NKI kernels, an inductive proof,
  Stellar, Firedancer.
- [For agents](agents/index.md): the workflow, the rules, the gates.
