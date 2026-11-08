---
template: home.html
title: sea-mlir
hide:
  - navigation
  - toc
---

<p class="lead">A new tile size changes the addresses a kernel computes. Parallel transaction processing changes the order in which ledger updates arrive. The intended result can stay simple even as the implementation grows harder to follow. sea-mlir helps an agent turn that intended result into a check you can run again after the next change.</p>

See what that takes in real code:

- [NKI kernels](tutorials/nki.md): does a tiled kernel move and update the
  right tensor elements for every allowed index vector?
- [Stellar](tutorials/stellar.md): does merging parallel ledger updates
  preserve the largest requested TTL?
- [Firedancer](tutorials/h2-flow-control.md): do HTTP/2 flow-control updates
  preserve the protocol's credit accounting?

An agent supplies the harness, property and extraction into a small MLIR
dialect. sea-mlir checks the resulting program with an SMT solver. A
completed check establishes the property under its assumptions or returns
a counterexample that the interpreter replays. A solver can also answer
`unknown`, including when it reaches a resource limit.

## Assembly for verification

Automated verification often stalls on encoding, not on the program. When
a proof is slow or fails, someone has to find what makes the problem hard
and change how it is posed: the memory model, the decomposition, the
assumptions, the solver settings.

Tuning that encoding at source level can be awkward: a compiler front end
stands between an edit and its effect on the formula. A small source
change can produce a large change in the encoding. Working directly on
SMT constraints has a different cost. Expanded memory and arithmetic
constraints can obscure the program structure that explains them.

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

The agent saves the harness, extraction command, assumptions and expected
results, together with a profile of solver and encoding options. CI
regenerates the verification jobs from each source revision and runs them
with that profile. When a check fails or stops finishing, the agent can
inspect the change and revise the proof setup. The aim is to reuse the
verification work across ordinary code changes.

## How it fits with other tools

sea-mlir makes the verification program an artifact that an agent can
inspect, transform and run. Its role is easiest to see at the boundaries:

- **Source front ends** extract the code to check. SeaHorn exports C
  harnesses; the NKI study uses a Python tracing harness. The extraction
  determines which source behavior reaches the verifier.
- **Verification languages** provide a place to state proof obligations.
  The `sea` dialect concentrates on low-level operations: control flow,
  bit-precise arithmetic, pointers and byte-addressed memory. Its
  executable semantics also make counterexample replay possible.
- **SMT solvers** discharge those obligations. sea-mlir controls the
  encoding and simplification, retains the program behind the formula,
  and connects the solver's model to a concrete execution.
- **Proof assistants** help establish the foundations. We use Lean for
  SeaM's memory semantics and rewriting rules. Application checks instead
  use the saved harness and solver configuration to discharge new
  obligations automatically. Both the harness and the configuration can
  need maintenance as the application changes.

## Why trust the results

Counterexamples are replayed as part of an ordinary verification run.
With `--seam-check`, the solver also checks each memory rewrite against
its premises. Development QA compares independent components through
operation tests, differential runs against SeaHorn, and fuzzing with
mutants. Lean proofs establish the soundness of the formalized rewriting
rules; conformance checks connect those rules to production terms.

These checks cover different parts of the tool. They do not amount to an
independent certificate for every successful proof. [Why trust it](trust/index.md)
explains what runs by default, what can be enabled, and what the formal
proofs cover.

## Start here

- [Get started](guide/getting-started.md): build, run a first proof, read the output.
- [Applications](tutorials/index.md): NKI kernels, an inductive proof,
  Stellar, Firedancer.
- [For agents](agents/index.md): build a harness, explore an encoding, and preserve the check in CI.
