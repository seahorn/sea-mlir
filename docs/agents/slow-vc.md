# Diagnosing a slow or timed-out VC

A job that times out, or that became several times slower after a change,
has a cause in the query or in the search. Timing says that something is
wrong, not what. The steps below find where the cost comes from before
anything is tuned. Each step lists commands that exist today. Run a
solve only when its answer changes the next step.

## 0. Which kind of slow

- A regression: the same job was fast before a change of the tool, the
  program, the extraction or the solver. Keep both versions; go to step 2.
- Always slow: there is no earlier version. Go on with step 1 and then
  step 3.

Also ask what code the query contains and whether it can run. A dead
branch or an unreachable parser still contributes reads and Ackermann
constraints; `--reach=blocks` with a small rlimit reports blocks that are
unreachable at once. Seed variance of 5x or more on a `sat` job points the
same way: the solver is lost in terms that do not matter.

## 1. Size or search

Write the query once and give it to z3 directly, with verbosity and
statistics:

```bash
sea-vc job.mlir <the job's flags> --smt2 > q.smt2 2> sea.err
z3 -v:10 -st q.smt2 > z3.log 2>&1
```

`sea-vc` accepts the same switch as a global parameter
(`--z3-param=verbose=10`), so the line also appears inside a normal run.

- `-v:10` prints one line per tactic step with `num-exprs` and the memory
  (`bit-blast`, `ackermannize_bv`, `max-bv-sharing`, `aig`, ...). The
  number after `bit-blast` is the size of what the SAT solver gets.
- `-st` prints conflicts, SAT variables and clauses at the end.

Large `num-exprs` after `bit-blast` with few conflicts: the cost is the
encoding. Few expressions and many conflicts: the cost is the search.
Compare the two runs of a regression the same way: the table of both
decides which of the two it is, without a third solve.

Read the step list for steps that did not happen or did nothing:

- z3's QF_BV tactic runs `simplify; solve-eqs; aig` after `bit-blast`
  only when the memory is under 300 MB. A query that is just over the
  limit loses the step silently: the line is absent.
- A step whose `num-exprs` does not change (here `max-bv-sharing`) did not
  do its job.

Both are findings. Read the solver source for the step and its history
(z3's `src/tactic/smtlogics/qfbv_tactic.cpp` and the step's own file);
the mechanism is often a guard or a parameter that no longer does what its
name says.

Confirm which engine ran. `sat` is the bit-blasting path of the QF_BV
tactic; `smt` is the general solver. A `sat-status` or `smt` line in the
`-v:10` log says which. A trace and verbose-log analyzer such as lemur
(a separate tool, not part of Sea-MLIR) prints the per-step table and the
engine for a run (`lemur run-report`); the log above has the same
numbers.

## 2. A regression: which part of the change

Do this before looking into the query. A change that makes a job slower
often exposes a weakness that was already there.

1. Bisect the commits of the change, then the edits inside the commit
   that is responsible. A pass that makes many small edits can be bisected
   with a temporary limit on the number of edits; the effect may be the
   sum of many edits and not one of them.
2. Then find the mechanism: what in the query changed. Compare the
   programs:

   ```bash
   sea-vc old.mlir <flags> --print-lowered=old.low.mlir --vcgen-only
   sea-vc new.mlir <flags> --print-lowered=new.low.mlir --vcgen-only
   diff old.low.mlir new.low.mlir
   ```

   and the queries: `utils/encoding-diff.py` compares the queries of two
   programs under one `sea-vc`; `utils/query-diff.py` compares the queries
   of two `sea-vc` binaries on one corpus. Both read the printed queries,
   so they can be slow on queries with deep terms; start with a reduced
   program (step 4).

A smaller query can be slower. Do not revert a change on one timing; find
the mechanism and check it with several seeds (step 6).

## 3. Measure the query with sharing

Printed SMT-LIB is unshared: the same subterm is written out at each use.
Operator counts, sizes and file sizes of the printed query mislead (a
printed goal showed four times the multiplications that the shared query
has). Count on the DAG:

```bash
sea-vc job.mlir <flags> --dag-stats --vcgen-only
```

prints `vc.dag-size` and the distinct nodes per operator. z3's
`num-exprs` also counts shared nodes, per formula after each step.

Then attribute the large terms to program values:

- `--arith-stats`: multiplications, divisions and remainders by width and
  operand kind, per source location of the sea op that made them.
- `--ack-dump-pairs=<file>`: every lowered read, with the locations of the
  loads, and every Ackermann pair; classify the pairs (same object,
  different objects, constant offsets).
- `--eq-audit`: the pairs left undecided, by the layer that could decide
  them, and the most frequent shapes.
- `--ite-stats`: the memories, addresses and integers that are ite trees.
- Origins: nodes record the location of the sea op that made them
  (`include/sea/VC/Origins.h`); the reports above print it as `loc=`.

## 4. Reduce on a deterministic metric

Reduce the program with `sea-reduce` ([Minimizing a problem](minimizing.md)),
with an interestingness test on a number and not on time. `slow` mode of
`utils/reduce-test.py` takes `--metric time|mem|<counter>`, where a
counter is a `--seam-stats` line; prefer a counter. For a quantity of the
solver phase, such as the ratio of the `bit-blast` `num-exprs` to the
`ackermannize_bv` `num-exprs` in the `-v:10` log, write the test: run z3
until the line of the phase is printed, then stop it.

```bash
#!/bin/bash
# t.sh <file>: exit 1 (interesting) when the bit-blast is R times the
# size before it; any failure is uninteresting.
f=$1; R=${R:-5}; MIN=${MIN:-10000}
sea-opt "$f" > /dev/null 2>&1 || exit 0
sea-vc "$f" --lower --seam --widen-memory=4 --smt2 2> /dev/null > "$f.smt2" || exit 0
timeout 300 z3 -v:10 "$f.smt2" 2>&1 | awk -v R="$R" -v MIN="$MIN" '
  /^\(ackermannize_bv / { a = $3 }
  /^\(bit-blast /       { b = $3; if (a > 0 && b >= R * a && b > MIN) r = 1; exit }
  END { exit r ? 1 : 0 }'
exit ${PIPESTATUS[1]}
```

The pipe ends z3 at its next write after awk exits. Use `timeout` as a
safety limit, not as the metric. `utils/reduce-test.py script` wraps a
built-in mode the same way, with the parse check first; the sketch does
the same with `sea-opt`. Start from the optimized program: the lowered
program (`--print-lowered=low.mlir`) can be read back without `--lower`
(pass the other flags again), and a reduction of it keeps what makes the
query hard and not the rewrites that produced it.

## 5. Isolate with small parametric examples

When a mechanism is suspected, write a generator for a family of small
programs or queries that has the mechanism and a size parameter, and
measure the metric of step 4 over the parameter. An example of a few
dozen lines gives an answer in seconds, where the real query takes
minutes, and each candidate fix can be run on the family before the real
job.

## 6. Controls

- Other solvers: `--solver=yices`, and `--solver=bitwuzla
  --ackermannize=on`. A solver that is not slow on the query points at the
  step that z3 lacks or gets wrong; its source often shows the guard.
- Seeds that matter. For a QF_BV query solved by bit-blasting, the seed is
  `sat.random_seed`; also set `smt.random_seed`:
  `--z3-param=sat.random_seed=3,smt.random_seed=3`. Run several seeds
  before calling a difference real.
- Traces: z3's `-tr:<tag>` prints what a component did; the tags are in
  z3's `src/util/trace_tags.def`. Needs a z3 built with tracing.
- `--z3-tactic=<t>` and `--z3-param=key=value` change one thing of the
  solver. Use them to test a mechanism that the steps above named, not to
  search for a setting that happens to be faster.

## 7. Fix by the mechanism

- Prefer the general fix over one for the job in hand, and measure that
  the general fix does not need option search.
- A name or abstraction that the encoding adds for the solver must survive
  the solver's own simplification. z3's `solve-eqs` eliminates a constant
  defined by a top-level equation and substitutes it back: the query
  is then as it was. `--vc-defs=hidden` defines names by two `bvule` atoms
  each, which `solve-eqs` does not turn into a substitution
  ([Named address sums](../design/SumNames.md)).
- A fix that rests on an assumption goes behind its own switch, off by
  default, and the assumption is counted and listed with the others
  (`assume.*` lines). `--narrow-offsets` rests on A-INB
  ([Ackermann](../design/Ackermann.md), "Narrowed offsets").

## 8. Record the case

Add a regression test that pins an outcome of the encoding that the fix
changes: for example the number of Ackermann constraints
(`vc.ackermann-constraints`) or the width of the compared terms of a
small example from step 5. Do not pin the count of rule firings; it
changes with every rewrite and says nothing about the cost.

## Worked example: cursor sums in Ackermann pairs

Jobs `txn-pf-v1-writes` and `txn-reach-v0-alt` ([the table](../tutorials/txn-parse.md))
became 3-6x slower under z3 after `sea-prune-edges` was added to the
lowering.

1. Kind: a regression. Both versions of the program were kept.
2. Size or search: `-v:10` gave 1.4M expressions after `bit-blast`
   before the change and 7.3M after it; `-st` gave 75k and 84k conflicts.
   The cost was the encoding, not the search. After the change the `aig`
   step was absent (the memory was over 300 MB), and `max-bv-sharing`
   changed nothing.
3. Bisect: the effect was the sum of many of the pass's edits, not one.
   The pass removes the edges into the error-exit joins (the paths that
   only end in a failed check), and the join block arguments that carried
   the parser's cursor went with them. Without them the cursor became a flat 64-bit
   sum of the cursor's increments in every read address: sums of up to 34
   operands.
4. Measure with sharing: the printed goal showed 27k multiplications
   against 5.7k, which was misleading; counted on the DAG, both queries
   had 37k nodes and the same multiplications, and the difference was
   the arity of `bvadd`. The wide sums were the index sums of same-object
   Ackermann pairs (found with ad-hoc DAG scripts at the time;
   `--ack-dump-pairs` and `--eq-audit` list the pairs). For each pair z3
   cancels the common prefix of the two sums and bit-blasts what is left,
   so the same prefix is blasted for each of thousands of pairs.
5. Controls: Yices and Bitwuzla were not slower (each keeps sums shared
   before bit-blasting). Reading z3 showed two steps of its QF_BV preamble
   that do nothing: `max-bv-sharing` never registers the operators it
   shares, and `solve-eqs` never counts occurrences, so its `max_occs`
   bound (2 in the preamble) does not stop it from eliminating names
   (a trace, `-tr:solve_eqs_check_occs`, shows `num_occs: 0` everywhere).
6. Fix by the mechanism. `--vc-defs=hidden` names the shared prefixes with
   constants that `solve-eqs` does not eliminate, which brought the
   `txn-pf-v1-writes` bit-blast from 7.34M to 2.27M, below the limit for
   `aig`. The pairs themselves remain: 39k pairs of 64-bit equalities
   keep `txn-reach-v0-alt` at 8.9M. `--narrow-offsets` compares two reads
   of one object at k bits (2^k at least the object's size) and rests on
   A-INB, so it is a separate switch, counted with the assumptions.

The measurements are in [Named address sums](../design/SumNames.md).
