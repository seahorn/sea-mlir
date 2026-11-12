# Application verification with agents

An agent turns a property worth preserving into a check that can run again
when the application changes. It writes or adapts the harness, extracts
the relevant code, checks the result and controls, then adjusts the
encoding when needed. The deliverable is the whole verification package:
a contract tied to source, an extraction recipe and a solver configuration
that CI can rerun.

This page describes that application workflow. Agents changing Sea-MLIR
itself should also follow [Sea-MLIR contributor gates](gates.md) and
[contributor rules](rules.md).

## The workflow

### 1. Choose the property and write its contract

Start with a concrete consequence of failure and a reason to expect the
code to change. A data-movement refactor should preserve which input cell
reaches each output cell; a parallel merge should preserve every legal
TTL bump. The [application tutorials](../tutorials/index.md) show these
choices and their engineering motivation.

Write the harness and state what it checks, the allowed inputs, and its
assumptions. Record any model, mock, hand port or loop bound that limits
the claim. Include the caller facts needed to interpret a failure as an
application defect. An assumption that makes the problem tractable is
part of the contract, not a hidden solver setting.

### 2. Extract the selected source

Record the application revision, harness revision, frontend versions and
exact extraction command that produce `job.mlir`. Check that extraction
uses the intended source and compile-time configuration. Treat a failed
export as a failed job; do not continue with an old `.mlir` file left by
a previous run.

Each Sea-MLIR tool prints a terse guide to its options, output lines and
exit codes:

```bash
sea-vc --agent
```

Read it once per session before running the tools
([Running the tools](running.md)). To steer `sea-run --fuzz` read
[Steering the fuzzer](fuzzing.md). The guide is maintained with the code.

### 3. Run the controls, then tune

Run the proof together with its [controls](../guide/proof-controls.md):
reach jobs that admit the intended executions, and mutants or historical
reverts that the property should reject. A fast proof is useful only if
it checks the intended behavior. Keep the machine-readable output and
set limits for each run:

```bash
sea-vc job.mlir --lower --seam --widen-memory=4 \
    --timeout=900 --mem-limit=16 --format=jsonl > run.jsonl
jq -r 'select(.kind=="result").value' run.jsonl
jq -c 'select(.kind=="stat" and .key=="seam.inb")' run.jsonl
jq -c 'select(.kind=="replay") | {status, at, path}' run.jsonl
```

Check the exit status against the result, and read stderr for warnings.
When a run is slow or does not finish, look at where the work goes:

- `--vcgen-only` builds the query without solving it and prints its
  statistics;
- `--seam-stats` prints how often each rewrite rule fired;
- `--progress` logs each phase with its time and sizes (`'--log=*:1'`: the
  whole flow);
- `vc.ackermann-constraints` measures the memory work left to the solver.

Then change one thing and compare: the memory encoding (`--seam`,
`--widen-memory`), the solver (`--solver=yices`, `--solver=bitwuzla`), a
z3 parameter (`--z3-param`), or the harness itself (a loop cut, a split of
the claims into separate jobs). Compare counters before times: counters
are deterministic. When a problem is too large to study, reduce it first
([Minimizing a problem](minimizing.md)).

### 4. Save the verification package

Save the harness, its property and assumptions, the source revision, the
extraction command and tool versions, the expected verdicts, and the
controls. Write the options that worked into a profile file:

```text
# fd-reasm.cfg: the configuration of the reassembly proofs
--lower
--seam
--widen-memory=4
```

```bash
sea-vc job.mlir --config=./fd-reasm.cfg --timeout=900 --mem-limit=16
```

Every run prints the profile files it loaded (`vc.config:`), so the
result records its configuration ([Profiles](profiles.md)). A profile
records solver and encoding options; it does not establish which source
revision a `.mlir` file represents.

### 5. Regenerate and check in CI

On each revision of the application, **regenerate the verification jobs
from that revision before solving them**. Run the proof and controls with
the saved profile, expected verdicts and resource limits. Keep the
source/extraction provenance with the results so that a passing run names
the code it checked.

An implementation change can reuse the contract and configuration. A
change to the property, assumptions, bounds, mocks or extraction needs
review of the claim itself; do not accept it merely because the new job
passes. If a verdict changes or a job exceeds its limits, investigate the
new source and controls before changing the expected result or weakening
the contract.

Reusing a fixed exported `.mlir` job is appropriate when testing a change
to **Sea-MLIR**: the question is whether the verifier still handles the same
input correctly and efficiently. It does not check a new revision of the
application. The [verifier benchmark suite](../reference/benchmarks.md)
uses fixed jobs for that purpose.

## What an agent reports

For each result: the application and harness revisions, the extraction
command and frontend versions, the `tool.version` line, the full command
with every `vc.config`, the exit status, the `result`, the assumption lines
(`seam.inb`, `seam.ptrint`, `assume.memset-bound`, `assume.a-inb`,
`assume.addr0-is-null`), the warnings, and for a `sat` the
replay status and `assert-at`. For a timing claim: the machine, and
whether other jobs were running.

## What an agent does not decide

- Whether an assumption is acceptable. The agent reports it; a person
  decides.
- Whether a `sat` is a bug of the system. A `sat` from a harness with
  havocked inputs is a statement about a function's contract until the
  callers' invariants are in the harness ([Stellar TTL merge](../tutorials/stellar-ttl.md#the-local-contract-can-expose-unreachable-inputs)).
- Whether an oracle may accept something new ([Sea-MLIR contributor rules](rules.md)).
