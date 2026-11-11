# Running the tools

## Read the guide first

Every tool has `--agent`: a plain-text guide to what the tool does, why to
use it instead of reasoning by hand, typical commands, output lines and
exit codes.

```bash
sea-vc --agent
sea-run --agent
sea-opt --agent
sea-reduce --agent
sea-seam-export --agent
sea-reduce-test --agent
```

In a source tree, `python3 utils/bench.py --agent` describes the
benchmark harness.

## Which tool for which question

| question | tool |
|---|---|
| Does the assertion hold for all inputs? | `sea-vc` |
| What does the program do on these inputs? | `sea-run --input=<site>=<value>,...` |
| Which inputs get past an assume, and how far do random runs get? | `sea-run --fuzz` ([Steering the fuzzer](fuzzing.md)) |
| Is the program well formed? What does a pass do to it? | `sea-opt` (`--sea-vc-lower` is what `sea-vc --lower` runs) |
| What does the solver see? | `sea-vc --smt2` |
| How large is the query, without solving it? | `sea-vc --vcgen-only` |
| Which rewrites did the work? | `sea-vc --seam-stats` |
| How do I make a large failing case small? | `sea-reduce-test reduce` ([Minimizing a problem](minimizing.md)) |

Evaluating bit-vector arithmetic or tracing block arguments by hand gets
the corner cases wrong: division by zero, signed remainder, shifts by the
width or more, wrap-around. Run `sea-run`.

## Limits

`--timeout` bounds the solver only. Query generation (lowering, encoding,
SeaM rewriting) is not bounded by it, and large exports have needed tens
of GB. Set an external timeout and a memory limit as well:

```bash
timeout 900 sea-vc job.mlir --lower --seam --widen-memory=4 \
    --timeout=900 --mem-limit=16 --progress
```

- `--mem-limit` is in GB of address space (`RLIMIT_AS`), not resident
  memory. Leave headroom above the expected resident size.
- `--progress` (the same as `--log=vc.phase`) logs the start and end of each
  phase on stderr, or the log file, the end with its time and sizes, so a run that does not
  finish shows where it is. `'--log=*:1'` adds the passes of the lowering
  and the solver calls ([Logging](../reference/logging.md)).
- A limit that is hit gives `result: unknown` and exit status 3, with
  `solve-timeout: true` or `mem-limit: hit`.

On a shared machine, run a few jobs in parallel at most.

## Machine-readable output

`--format=jsonl` (`sea-vc`, `sea-run`) prints every stdout line as one JSON
object with a `kind`, typed fields, and `text`, the text line it replaces.
Diagnostics stay on stderr as text.

```bash
sea-vc p.mlir --format=jsonl | jq -r 'select(.kind=="result").value'
sea-vc p.mlir --format=jsonl | jq -c 'select(.kind=="replay") | {status, at, path}'
sea-vc p.mlir --format=jsonl | jq -r 'select(.kind=="print") | [.path, .repr] | @tsv'
sea-vc p.mlir --seam-stats --format=jsonl \
  | jq 'select(.kind=="stat" and (.key|startswith("seam.rule."))) | {(.key): .value}'
```

The record kinds are listed in
[Output formats](../reference/output-formats.md#jsonl).

## Diagnostics

Diagnostics go to stderr. `Warning: <message> [kind]` names incomplete or
approximate handling; each kind is printed once and repeats are summarized
at exit (`--log=warn.<kind>:2` prints each; see
[Logging](../reference/logging.md)). `Error: <message> [kind]` stops the
tool. The kind is a short stable tag, such as
`vc.seam-inb-violated`; match on it, not on the message. Nothing
unsupported is skipped silently: an operation, layout or form that
`sea-vc` cannot encode exactly is an error with exit status 1.

## Exit status

| status | `sea-vc` |
|---|---|
| 0 | `unsat`, or `sat` confirmed by replay: the run fails an assertion, or with `--ptr-companion=check` a split pointer load fails its A-PTRINT guard |
| 1 | error: bad input, not in BMC form, unsupported operation, replay impossible |
| 2 | `sat`, but the replay disagrees with the model (a value differs, or the run shows no failure: `vc.replay-no-failure`): a bug |
| 3 | `unknown`, including a timeout or a memory limit |
| 4 | `--seam`: a `sat` whose replay leaves an object; the answer is inconclusive and the program has a memory-safety error |
| 5 | `--seam-check`: an invalid rewrite; a bug, do not trust the verdict |

Exit statuses 2 and 5 are always bugs in sea-mlir, and so is a crash or a
`<pass>.postcondition` error. Reduce and report them; do not interpret the
verdict. The other tools' codes are in
[Output formats](../reference/output-formats.md#exit-codes).
