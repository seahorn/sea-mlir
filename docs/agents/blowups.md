# Diagnosing a VC blowup

A job that dies with `LLVM ERROR: out of memory`, or that `bench.py`
classifies as `memout`, ran out of memory while it built the query or while
the solver ran. `--progress` names the phase. The steps below find what
grows inside that phase, which rule makes it grow, and on which reads and
stores, without waiting for the out-of-memory kill.

## 1. Stop early, with a census

Run the job's command with a heartbeat and a budget:

```bash
sea-vc job.mlir <the job's flags> --vcgen-only --progress \
    --mem-trace --mem-top=12 --max-rss=8000 2> trace.err > trace.out
```

- `--mem-trace` (the same as `--log=vc.mem`) writes a `vc.mem beat` line on
  stderr every `--mem-trace-interval` seconds (default 10) while terms are
  made, and a `vc.mem phase-end` line at the end of each phase. Each line is
  written and flushed as it happens, so a run that is killed keeps them.
- `--max-rss=<MiB>` (resident memory) or `--max-nodes=<n>` (live terms)
  turns the out-of-memory kill into a stop: `result: unknown`, a
  `budget: hit` line, the census of that moment (`budget.*` lines), error
  `vc.node-budget` or `vc.rss-budget`, exit status 8 (own status: not the 3
  of a timeout or `--mem-limit`). `--max-nodes` is
  deterministic: the same budget stops at the same term on every run, so
  two variants of a job can be compared at the same size. Give both
  budgets: the terms of some configurations are large (under
  `--seam-row-group=0` an n-ary `||` node averages about 2.5 KB, so 5 M live
  nodes reached 14 GB and an out-of-memory kill before the node budget). The budgets
  bound encoding and solving: once the solver has answered `sat` or
  `unsat`, or `--reach` has decided its targets, they are off, so a budget never replaces a verdict (the replay
  of a `sat` is bounded by `--mem-limit` only). `--max-rss` needs
  `/proc/self/statm`; where it cannot be read the run stops at startup with
  error `vc.rss-budget-unsupported`. The parts of `--check-inb` inherit
  `--max-nodes`, `--max-rss` and `--mem-trace`; of the parts' statuses,
  8 ranks just above 3.
- `--vcgen-only` keeps the solver out when the blowup is in the query.

What a beat says (fields in [Logging](../reference/logging.md)):

| Field | Question it answers |
|---|---|
| `phase` | which phase grows (`encode` includes SeaM's rewriting of each load and store; `simplify`; `solver-term` is the solver boundary, which expands store-maps and memory operations) |
| `nodes-live`, `nodes-made` | live terms and terms made so far: a small live / made ratio means much is built and freed again |
| `rw-cache` | entries of the rewriter's cache; each keeps its terms alive |
| `top-ops` | which operators the live terms are |
| `top-rules` | which rules fired since the last beat |
| `op`, `op-at`, `op-nodes` | the operation being encoded and the terms it made so far |
| `top-sites` | the operations of `encode` that made the most terms |

The live terms only grow when terms are made, so the checks (every 4096
terms made) see every growth of the term graph. They do not see memory the
solver takes: that shows as `rss-mb` in the next `phase-end` line, and
`--mem-limit` bounds it.

## 2. Read the growth

Take two beats of one phase and divide. Rule firings since the last beat
(`top-rules`) against the terms made in between give the terms per firing;
the counts of two related rules give their ratio. A ratio that stays fixed
while a per-operation count grows separates the two factors of a product:
what one step costs and how many steps an operation takes.

`top-sites` with the same source position many times over (an unrolled
loop) and growing counts means the cost of one operation grows with the
program's length: the operation reads through a history that gets longer.

## 3. Test the hypothesis with a switch

Most SeaM rules have a switch (`sea-vc --help`, `--seam-*`). Rerun with
the switch that turns off the suspected rule and the same budget. A run
that now passes the phase and grows in a later one moves the question
there; the census says which phase and which operators.

## 4. Reduce

A smaller program with the same curve makes the analysis fast:
`--max-nodes` makes the interestingness test cheap and deterministic
(`budget: hit` within a phase). `utils/reduce-test.py` does not recognize
`budget: hit` yet: its `slow` mode with `--metric mem` accepts a kill,
`mem-limit: hit` or an out-of-memory message, so for a budget write a
custom interestingness test that looks for `budget: hit` in the output
([the reducer](../reference/reducer.md)).

## Example: an h2 flow-control job

`h2fc-blowup-tx-reach-ring` (public suite, `firedancer`), default
configuration `--lower --seam --widen-memory=4`, died after about 800 s at
11.9 GB resident, in `encode`. With `--mem-trace --mem-trace-interval=15`
(a 14-beat trace):

- The phase is `encode`, and the terms are alias questions: at 616 s,
  25.1 M live terms of 65.8 M made, 13.0 M `overlap`, 6.5 M `=`, 5.4 M
  `ite`; everything else is under 0.1 M. `rw-cache` holds 8.0 M entries.
- The rules: per beat, `row.smap-group` / `row.smap-abort` stays at about
  211, and `decide.pairs` near the same. Each R-ABORT (a read whose index
  has no single-object view, over a store-map whose entries stay
  undecided) expands about 211 open entries into an ite chain, one alias
  question per entry. That is about 1,500 terms made and 730 kept per
  R-ABORT, from start to end of the trace.
- The sites: the loads of one source position cost 0.48 M terms in the
  first iterations of the unrolled loop and 8.8 M in the late ones. The
  entries per R-ABORT stay at 211; the R-ABORTs per load grow from about
  330 to about 6,000: one load reaches more and more store-maps, the
  memory versions joined into its memory (`row.ite`, a read pushed into
  both sides of a memory ite, fires about 0.3 times per R-ABORT). The
  program has about 3,400 store-map versions in all (the run with
  `--seam-row-expand=0` below), fewer than the R-ABORTs of one late load.
  A first hypothesis, one read per word and index leaf, does not hold:
  the memory is byte-indexed, so an 8-byte load makes 8 byte reads
  whatever the word size, a constant factor.
- So the size is reads x memory versions x open entries: the expanded ite
  chains of two versions are different terms, and the read index differs
  from one load to the next, so nothing is shared across loads.
- The cause the recipe led to: the ring buffer is moved into `tx_buf` by
  `memmove`, and the range tests of those moves are undecided on
  destination leaves that have no single-object view. The branch that hits
  reads a multi-object index that is dead there, and the read then
  R-ABORTs through every older memory operation and store-map. The terms
  grow with the number of memory versions behind the load, not with the
  loads.
- The test: `--seam-row-expand=0` (keep such a read as a select over the
  store chain) finishes `encode` and `simplify` in 1.3 s with 0.15 M terms.
  The growth moves to `solver-term`: `--max-rss=12000` stops it after 53 s
  with 33.5 M live terms (17.1 M `ite`, 9.6 M `=`, 2.2 M `bvult`, 1.7 M
  `select`, 1.6 M `ptr-diff`): the boundary's read equations of the memory
  operations, again per read and per operation.
