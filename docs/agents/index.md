# For agents

sea-mlir is built to be driven by agents. An agent writes or adapts a
harness, runs `sea-vc`, reads the result, changes how the problem is posed,
and runs again. When a configuration works, the agent saves it as a
profile, and later revisions of the code are checked with that profile
without the agent. This section describes that workflow and the rules
that go with it. A person directing an agent can read it as the contract.

## The workflow

### 1. Learn the tools

Each tool prints a terse guide to its options, output lines and exit
codes:

```bash
sea-vc --agent
```

The guide is kept in sync with the code, so it is more current than this
site. Read it once per session ([Running the tools](running.md)).

### 2. Explore

Run the job, with limits, and keep the machine-readable output:

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
- `--progress` prints the phase the run is in;
- `vc.ackermann-constraints` measures the memory work left to the solver.

Then change one thing and compare: the memory encoding (`--seam`,
`--widen-memory`), the solver (`--solver=yices`, `--solver=bitwuzla`), a
z3 parameter (`--z3-param`), or the harness itself (a loop cut, a split of
the claims into separate jobs). Compare counters before times: counters
are deterministic. When a problem is too large to study, reduce it first
([Minimizing a problem](minimizing.md)).

### 3. Save the configuration

Write the options that worked into a profile file:

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
result records its configuration ([Profiles](profiles.md)).

### 4. Hand it to CI

Commit the profile with the harness, and run each job with
`--config=<profile>` on every new revision of the code under test. A
revision that keeps the expected verdict needs no further work. A revision
that changes a verdict, or that no longer finishes within its limits, goes
back to step 2.

## What an agent reports

For each result: the `tool.version` line, the full command with every
`vc.config`, the exit status, the `result`, the assumption lines
(`seam.inb`, `assume.memset-bound`), the warnings, and for a `sat` the
replay status and `assert-at`. For a timing claim: the machine, and
whether other jobs were running.

## What an agent does not decide

- Whether an assumption is acceptable. The agent reports it; a person
  decides.
- Whether a `sat` is a bug of the system. A `sat` from a harness with
  havocked inputs is a statement about a function's contract until the
  callers' invariants are in the harness ([Stellar](../tutorials/stellar.md)).
- Whether an oracle may accept something new ([Rules for contributors](rules.md)).
