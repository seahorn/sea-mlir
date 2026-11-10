# Sea-MLIR contributor rules

These rules apply to anyone who changes sea-mlir or reports its results,
person or agent.

## Conditional soundness

Verify under named assumptions; never drop one silently.

- An encoding may rely on an assumption, such as the in-bounds assumption
  of SeaM (every access stays inside its object), if the assumption is
  named, reported in the output, and discharged by a separate check, such
  as a bounds-checking job.
- Do not discharge an assumption by verifying again without it. If the
  discharging check is missing, build it.
- Report every assumption a verdict rests on: `seam.inb: assumed ...`
  next to `result: unsat`, and also `assume.memset-bound`,
  `assume.a-inb` (memhavocs the lowering dropped under A-INB, also
  without `--seam` and with `--seam-inb=0`), `assume.addr0-is-null`
  (`vc`: placement makes null address 0; `uses=<n>`: a replay relied on
  it), `seam.ptrint`, and any `seam.inb: off (fallback ...)` or
  `seam.inb: off (lowering: ...)`.
- Front-end annotations (`align`, and the `inbounds`, `nusw` and `nuw`
  flags of pointer arithmetic) are trusted premises. `--check-align` and
  `--check-ptr-index` are separate jobs that assert every such promise;
  run them when the result matters.

The principle is from Conway, Dams, Namjoshi and Barrett, *Pointer
Analysis, Conditional Soundness, and Proving the Absence of Errors*, SAS
2008.

## Unsupported input is an error

- Do not skip, approximate or work around an unsupported construct in
  silence. The tools stop with `Error: ... [kind]`. A harness or a pass
  that hides such an error turns an unknown into a wrong answer.
- Warnings (`Warning: ... [kind]`) name incomplete or approximate
  handling. Report them with the verdict.

## Counterexamples are runs

- A `sat` counts only when it replays: `replay: assert-failed` and
  `validate: ... mismatches=0`.
- Exit status 2 (the replay disagrees with the model) and 5 (an invalid
  rewrite) are bugs in sea-mlir. So is a crash, or a
  `<pass>.postcondition` error. Reduce and report them; do not interpret
  the verdict.
- Exit status 6 (`--replay-original`): the program before the lowering
  does not fail the assertion of the lowered program's confirmed `sat`. A
  pass of the lowering changed the program, unless the program depends on
  where placement puts its objects. Reduce and report it.
- Exit status 4 under `--seam` means that the program leaves an object on
  the model's inputs: a memory-safety error, and an inconclusive answer.

## Proofs need controls

- Every proof comes with reach jobs (vacuity checks, expected `sat`) and
  mutants (expected `sat`). An `unsat` without them can be vacuous; a loop
  longer than the unroll bound once made a whole step trivially `unsat`
  ([HTTP/2 flow control](../tutorials/h2-flow-control.md#vacuity-in-practice)).
- A mutant must be run against the obligation that catches it, and a
  proof-mode mutant must check the invariants, not assume them
  ([transaction parsing](../tutorials/txn-parse.md#a-constant-the-proof-explained)).
- [Proof controls](../guide/proof-controls.md) describes vacuity twins,
  reach jobs, mutants and check classes, and how to judge their answers.

## Tests and oracles

- An oracle checks meaning against an independent reference: operation
  semantics against z3, the query against the interpreter, static
  placement against the abstract interpreter, widened memory against byte
  memory. Oracles are listed in `test/README.md` and marked `ORACLE` in
  their header.
- Do not weaken an oracle to make it pass. Changing what an oracle accepts
  (its reference, its expected verdicts, its tolerance) needs a
  maintainer's review. Fixing the code so that the oracle passes does not.
- Other tests are disposable. A test that blocks a change is recreated
  against the new code, disabled with a reason, or removed when its
  property no longer exists. The commit message names each such test.
- A test checks a property, not the whole output: match
  `status: assert-failed`, not the full report, and do not pin step
  counts, SSA names or message wording.
- A benchmark's expected verdict is an observation. One that disagrees
  with the harness author's expectation needs an explanation, not a silent
  record.

## Output and tool guides

- Tool output is line-oriented `key: value` text, and `--format=jsonl`
  gives the same as JSON. Parse those, not prose.
- A change to a tool's options or output updates its `--agent` guide in
  the same change.

## Licensing

- sea-mlir is under Apache-2.0 WITH LLVM-exception. Every code and test
  file starts (tests: ends) with the two-line SPDX header, copyright
  `2026 The sea-mlir Authors` and license `Apache-2.0 WITH LLVM-exception`
  (`utils/spdx.py` has the exact form). `AUTHORS` names the authors.
- `just spdx-fix` adds the header to new files; `just spdx` (and the lit
  suite) checks it. Files that cannot carry a comment are listed in
  `REUSE.toml`.

## Publication hygiene

- Keep source and documentation free of personal paths and unpublished review or study references; run `python3 utils/privacy-check.py` before committing.
