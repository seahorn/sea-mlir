# The pivot-step gate: what it certifies, what it costs

For a z3 developer who changes `src/math/lp/static_matrix.h` or
`static_matrix_def.h`. This page describes the gate as it is. How it was
built, and why the argument has the shape it has, is in
[z3-pivot.md](z3-pivot.md). The harness and its scripts are not public yet;
file names below refer to it.

- **Function:** `static_matrix<T, X>::pivot_row_to_row_given_cell`
  (`static_matrix_def.h:53-95` at z3 `e8b932052`), the step that adds a
  multiple of the pivot row to one other row of the tableau.
- **Certified:** for every column of the target row, the step leaves the
  textbook coefficient, a cell exactly when that coefficient is nonzero, the
  work vector reset and `m_cancelled` at 0; no index passes the row's size;
  the function's `SASSERT`s hold.
- **Cost per pull request:** 45 solver jobs on z3's unmodified headers,
  321 s at one job at a time, 85 s at four in parallel, 0.26 GB peak.
- **On trust:** two proof rules (stated, not machine-proved), three callee
  contracts (proved on 2x3 matrices only), the mock's assumptions (validated
  by exhaustive native runs up to 3x4, not on every pull request), a target
  row of at most 4 cells, numbers in GF(3), and the toolchain.
- **Status:** the jobs, the five result classes with their exit codes, and
  the runner `real/run_pr.sh` exist. A CI workflow and path triggers are
  proposed. The optional seam patch has not been sent upstream.

## How much code is under the gate

| | size |
|---|---|
| the function body | 43 lines, 42 of them non-blank |
| with the z3 helpers that are compiled and checked as real code: `addmul` / `mul` (4), the cell classes and their accessors (26), the number helpers of `lp_utils.h` including `is_zero` (6), `get_val`, `row_count`, `column_count` (9) | 87 non-blank lines of z3 source |
| the compiled function | 343 LLVM instructions: 61 loads, 52 branches, 65 calls |
| one job's program after inlining and unrolling (`rp-value`) | 6,934 lines, 602 loads, 366 conditional branches |

Not expanded into the check:

- **The three callees.** `remove_element` alone is 28 non-blank lines of
  link surgery. Here they are contracts. The contracts are proved in
  separate jobs on 2x3 matrices, on text cut from the z3 file.
- **The vector operations.** `operator[]`, `size`, `empty` and iteration are
  mocks.
- **The numbers.** GF(3) stands in for z3's rationals. The big-number code
  behind every `addmul` and `is_zero` on `mpq` is not exercised at all.

## The certified property

The call is `pivot_row_to_row_given_cell(i, c, j)`. `c` is a cell of column
`j`; it names the target row `ii = c.var()` and a position `p = c.offset()`
in it. Write `a(r,k)` for the coefficient of row `r` in column `k` before
the call, `a'` after, and `alpha = -a(ii,j)`.

### Preconditions

| | precondition | where it comes from |
|---|---|---|
| P1 | The matrix invariant: each row cell and its column cell point at each other; indices are in range; no row holds a column twice; no stored coefficient is zero. | **Assumed.** `is_correct()` checks less: it compares values, not the back links. The three callees keep P1 (contract jobs, 2x3). |
| P2 | `m_work_vector_of_row_offsets` is all -1. | The constructor and `add_column` set -1 (`static_matrix.h:120`, `:148`); this function restores it (Q3). Other writers are not checked. |
| P3 | `i != ii`, and `c` is a cell of column `j`. | The caller `pivot_column_tableau` passes `column.back()` and asserts `c.var() != piv_row_index`. |
| P4 | `a(i,j) = 1`. | `pivot_column_tableau` calls `divide_row_by_pivot` first. The caller in `dioph_eq.cpp` is not traced. Only the form of Q1 needs P4: the code removes `(ii, j)` whatever `a(i,j)` is. |
| P5 | `column_count() >= row_count()`. | **Assumed**; a tableau has a basic column per row. Needed because the `SASSERT` at line 56 compares the row index `ii` with `column_count()`. |
| P6 | For Q5 only: the basic column of row `ii` has one cell, in row `ii`, with coefficient 1. | **Assumed.** No invariant function of z3 states it. |

### Postconditions, for every column `k`

- **Q1 value.** `a'(ii,k) = 0` if `k = j`, else `a(ii,k) + alpha * a(i,k)`.
- **Q2 representation.** `a'(ii,k) != 0` exactly when row `ii` holds one cell
  of column `k`, at a position below the row's size, with that coefficient.
- **Q3 work vector.** `w[k] = -1`.
- **Q4 counter.** `m_cancelled = 0`.
- **Q5.** Row `ii` is not empty (the `SASSERT` at line 94), under P6. The
  `SASSERT`s at lines 56, 58 and 68 hold.
- **Cross links.** The link of every cell the step moves or adds is exact.
  This is certified through the contracts of `remove_element` and
  `add_new_element`, not by a job on the body.

### Internal facts the gate also pins

A change that keeps Q1-Q5 and breaks one of these still fails the gate.

- In the cleanup loop (lines 86-93), every cell above the scan index is
  nonzero. This is why the cell swapped into a freed slot needs no second
  look.
- From line 64 on, at every call the body makes, `m_cancelled` equals the
  number of zero cells in row `ii`.
- New cells (positions `>= prev_size_ii`) are nonzero, and every
  `rowii[k]` has `k` below the row's current size.
- Each callee is called with its precondition: `add_new_element` only for a
  column absent from the row; `remove_element` only on a cell of row `ii`.
- The call pattern: one `remove_element` and one scan before the loop over
  row `i`; `size()` read once; the reset loop reads positions
  `0 .. prev_size_ii - 1` in order; the cleanup loop reads downward.

### For which inputs

- Any matrix shape, any row and column indices (32-bit, unconstrained), any
  length of row `i`, any value of `m_cancelled` on entry.
- Row `ii` has **at most 4 cells on entry** (3 after the pivot cell is
  removed). Longer rows are not covered by any job.
- Numbers are elements of **GF(3)**. Cancellation to zero occurs, which is
  what the function's control flow depends on. Magnitude and big numbers are
  lost. The mock number type has no ordering and the build succeeds, so the
  function never orders numbers.

### Not certified

- Target rows with more than 4 cells on entry.
- The bodies of the three callees beyond 2x3 matrices (`add` and `scan` also
  2x4). The big-number branch of `remove_element` is never exercised.
- Rows other than `ii`; they change only inside the callees.
- The callers. A separate job checks the loop of `pivot_column_tableau`
  against this step's contract; it is not part of the per-PR tier.
- Rational arithmetic, and P1, P5 and P6 themselves: that every state z3
  reaches satisfies them.

## What a developer sees

### Triggers (proposed)

| a change to | runs |
|---|---|
| `src/math/lp/static_matrix.h`, `static_matrix_def.h` | Tier A |
| the body of one of the three callees (same files) | Tier A and the callee contracts |
| `lp_utils.h`, `numeric_pair.h`; `util/vector.h`, `util/rational.h`, `util/debug.h` (shadowed: see "Limits") | Tier A |
| the harness | Tier B |

No workflow implements these triggers. `real/run_pr.sh` is Tier A.

### Result classes and exit codes

Every job lands in one class. The run exits with the class of highest
precedence and prints one summary line, `GATE <CLASS> (exit N): ...`.

| exit | class | a job is in it when | what it asks |
|---|---|---|---|
| 0 | pass | it is as expected | nothing |
| 1 | violated | a proof is `sat` with a replayed counterexample; or a reach job or vacuity twin is `unsat` | the change breaks a stated claim: fix it, or redo the argument for the new behaviour |
| 2 | stale | the harness or the argument no longer fits the code; decided before any solver runs | the gate cannot judge the change until its owner updates the harness; the change is not thereby wrong |
| 3 | inconclusive | timeout, `unknown`, or the memory limit (300 s, 8 GB) | neither a pass nor a finding |
| 4 | infrastructure | a tool is missing, a build step timed out or was killed, the solver crashed | rerun, or fix the CI |

Precedence is stale, violated, infrastructure, inconclusive. A harness
that no longer fits the code makes every verdict of the run meaningless,
including a counterexample, so stale comes first. A replayed counterexample
is definite whatever other jobs did. Jobs that did not run say nothing.

The outputs below are from runs made for this page; paths are shortened.

**Violated.** Planted bug: line 76, `++m_cancelled;`, replaced by `;`.

```console
$ real/run_pr.sh
GATE VIOLATED (exit 1): 7 violated, 38 pass; a claim of the gate does not hold for this code (or a case it covers became unreachable)
  counterexample: python3 -I real/sweep_real.py rp-aggregate rp-effect-C rp-effect-M -j 1 --out OUT/real --show   (and 4 more: rp-guarantee, rp-interference, rp-script, rp-value)
== wall 312 s, exit 1
```

The printed command shows each counterexample. For `rp-guarantee`:

```console
$ python3 -I real/sweep_real.py rp-guarantee -j 1 --out OUT/real --show
rp-guarantee: sat  (BAD: verdict sat != unsat)  2.45s
  assert-at: pm_mock.h:114:21  (inlined at OUT/real/rp-guarantee/static_matrix_def.h:65:30 <- real/pm_main.h:42:5)
  print  pm.S0     4 : ui32
  print  pm.p      1 : ui32
  print  pm.d_in0  1 : si32
  print  pm.d_pos  2 : ui32
  print  pm.b_in   1 : si32
```

`pm_mock.h:114` is the check named "G-RS: the iteration on B adds [D
became zero] to m_cancelled". It was inlined at z3's line 65, the loop
header, where the iterator advances after one pass through lines 66-77. So
one iteration updated a cell of row `ii` to zero and the counter did not
move. The printed values are the pre-state: the
size of row `ii`, the position of the pivot cell, and whether the tracked
column has a cell in row `ii` (and where) and in row `i`. The job's
directory holds the exact `static_matrix_def.h` that was compiled.

**Stale.** Six things are checked before any solver runs:

1. clang compiles the harness with the code under the gate. A renamed or
   re-typed callee fails here, because the harness's explicit specialization
   no longer matches a declaration. The current harness on `a200baf44`,
   where `add_new_element` still takes three arguments: "pivot_real.cc:65:43:
   out-of-line definition of 'add_new_element' does not match any
   declaration in 'lp::static_matrix<rational, rational>'".
2. Type pins: each member the mock stands for exists and has its type. The
   pin tests for the member first, so its own message is the first
   diagnostic. The current harness on z3 before the counter existed:

   ```text
   GATE STALE (exit 2): 2 stale; the code changed in a way the harness or the argument does not cover: re-check the argument
     first stale reason (rp-value): real/pivot_real.cc:51:1: static assertion failed due to requirement
     'pm_pin_m_cancelled<lp::static_matrix<rational, rational>>()': pin m_cancelled: static_matrix::m_cancelled
     is missing or no longer unsigned: re-check the argument (RS, A-EQ, Q4) (clang; build.log)
   ```
3. The callee set: on clang's output, the functions the body calls are
   exactly the listed ones. Recorded for a body that calls a new member:
   "calls an unlisted function: ... `scan_row_strip_to_work_vector_from`".
4. The members the body touches are exactly `m_rows`, `m_columns`,
   `m_work_vector_of_row_offsets` and `m_cancelled`. An unrelated new member
   of the class does not stop the gate; one the function starts to use
   does. The gate written for the body without the counter, run on the
   commit that introduced it:

   ```text
   GATE STALE (exit 2): 46 stale; the code changed in a way the harness or the argument does not cover: re-check the argument
     first stale reason (rp-effect-U): STALE: the function now uses member m_cancelled, which the argument
     does not mention (check_ir.py MEMBERS: m_columns, m_rows, m_work_vector_of_row_offsets)
   ```
5. The export succeeds and leaves no call to an undefined function.
6. In Tier B, each planted bug is a patch with exact text and no fuzz. It
   may apply at an offset if its text is unambiguous and lies inside the
   function, so an edit that only shifts lines above the function does not
   stop it. The seam patch applies exactly or the seam route is stale.

**Inconclusive.** With the solver limit lowered to 1 s for two jobs:
`GATE INCONCLUSIVE (exit 3): 2 inconclusive; no verdict within the limits`.
At the default 300 s no Tier A job is near the limit. **Infrastructure**
(recorded, not re-run, with the compiler path pointing nowhere):
`GATE INFRASTRUCTURE (exit 4): 1 infrastructure; ...`.

## How it works

### What is compiled

The Tier A jobs compile z3's own `math/lp/static_matrix.h` and
`static_matrix_def.h`, unmodified, with clang 18 at `-std=c++20`, and
instantiate `static_matrix<mpq, mpq>`. This "overlay" route is the default
because it needs no change to z3.

| replaced | by | how |
|---|---|---|
| the containers (`m_rows`, a row, the work vector, the size of `m_columns`) | mock classes that store no matrix (`pm_mock.h`) | `util/vector.h` is shadowed by a declaration-only header; the mocks are explicit specializations |
| numbers (`rational`) | GF(3) by table lookup | `util/rational.h` shadowed |
| the three callees | their contracts | explicit member specializations of `static_matrix<mpq, mpq>` |
| `SASSERT` | a checked claim | `util/debug.h` shadowed |

No access control is relaxed.

### The idea

The mock tracks one column, chosen arbitrarily: its cell in row `ii`
(called D), its cell in row `i` (called B), its work-vector entry, and the
scalars the body reads. When the body indexes a row at D's position it gets
D. At any other position it gets a fresh cell with unconstrained content,
assumed to satisfy the same invariant that is being checked for D at that
kind of program point. Because the tracked column is arbitrary, what holds
for D holds for every column. The loop over row `i` is summarized: the mock
shows one iteration and replaces the others by "some new cells were
appended, some old cells were cancelled".

### One obligation, end to end

Claim: an iteration of lines 66-77 that updates an existing cell adds 1 to
`m_cancelled` exactly when the cell becomes zero.

z3, lines 73-76:

```c++
                T& c = rowii[j_offs].coeff();
                addmul(c, iv.coeff(), alpha);
                if (is_zero(c))
                    ++m_cancelled;
```

The mock's `operator[]` on row `ii` returns D when `j_offs` is D's position
and records the counter's value in `pm.d_mc0`. `addmul` and `is_zero` then
run on D as compiled from z3. When the loop advances, the mock's iterator
`++` checks (`pm_mock.h:114`):

```c++
      if (pm.d_upd) KIT_CHECK(PM_C_GA, "G-RS: the iteration on B adds [D became zero] to m_cancelled",
                              *pm.mc - pm.d_mc0 == (unsigned)pm_dzero());
```

- **Job:** `rp-guarantee` enables this class of checks: `unsat`, 3.4 s.
- **Vacuity twin:** the same program with every check off and
  `assert(false)` at the end: `sat`. Some run passes all of the mock's
  assumptions, so the proof is not empty.
- **Reach:** `rp-reach-2` asks for a run in which D is cancelled: `sat`.
- **Planted bug:** removing line 76 makes the job `sat` at this check.

### The obligations

Times are the solver job without build, from the 321 s run of Tier A.

| obligation | job | verdict | wall s |
|---|---|---|---|
| the seven cases of one column: updated, cancelled, moved as last cell, untouched, new, pivot cell, absent | `rp-effect-U` .. `-Z` | unsat | 1.6 to 3.2 |
| Q1-Q3 for one arbitrary column | `rp-value` | unsat | 3.8 |
| other columns' cells keep their invariant; D is only moved | `rp-interference` | unsat | 3.0 |
| call pattern, callee preconditions, no index past the size, Q4 | `rp-script` | unsat | 2.7 |
| the body's `SASSERT`s | `rp-sasserts` | unsat | 1.9 |
| D satisfies, before each call, what is assumed of other cells there | `rp-guarantee` | unsat | 3.4 |
| each step moves the counter by its cell's contribution | `rp-aggregate` | unsat | 2.5 |
| no run is cut by the loop unrolling | `rp-bound` | unsat | 2.0 |
| 14 vacuity twins; 17 reach jobs | `rp-reach-*`, `rp-interference-reach-*` | sat | 1.7 to 2.3 |

Six arithmetic jobs (`agg-*`, under 0.4 s each) derive the mock's three
counting assumptions and Q4 from the counter equality. They compile no z3
code, so they are not in Tier A.

### The trusted base

1. **Rule RC** (one arbitrary column; guarantees checked before relies at
   each call, by induction over the call order) and **rule AGG** (a counter
   equals a fold over the row if each step moves both alike), with seven
   side conditions. `ARGUMENT.md` states them with proof sketches. Neither
   is machine-proved.
2. **The mock's assumptions.** Each is derived in the argument from a check
   on D. Independently, a native build evaluates every one of them on real
   stored matrices at each call of the body: 8,496 runs at 2x3, 9,506,304
   at 2x4, 11,324,160 at 3x4, no failure. This is not repeated per pull
   request: the mock changes only when the harness does.
3. **The callee contracts.** Proved by SMT on an arbitrary 2x3 matrix that
   satisfies P1 (`add` and `scan` also at 2x4).
4. **The toolchain:** clang 18.1.8, the SeaHorn export (inlining, unrolling
   to a loop-free program), `sea-vc` 0.2.0, and the SMT solver. The solver
   is z3. A z3 bug that turned a `sat` into `unsat` would hide a violation.
   Every `sat` is replayed on the program, so the reverse error is caught.
5. **The memory premises.** `sea-vc` simplifies memory under three premises
   about the checked program: distinct objects do not overlap (A-SEP), no
   object sits at address 0 (A-NULL), and address arithmetic does not wrap
   (A-NOWRAP). Every pivot job reports `seam.premises: assumed`. The jobs
   pass `--seam-out-of-object=fallback`: if `sea-vc` finds an access that is
   statically outside its object, it drops the premises for that run and
   the verdict stays unconditional. The tool's default is to report such
   accesses and keep the premises, which would make the verdict
   conditional. No pivot job triggers the fallback.
6. **The bounds.** At most 3 old cells are shown to the two index loops.
   Loops are unrolled 4 times, which keeps 3 iterations; `rp-bound` shows
   no run of the body needs more.

## Cost

Measured in November 2026 on a shared 48-core machine, `sea-vc` 0.2.0.
Single runs; the load average at the start and end of each run is given.

### Tier A: every pull request that touches the trigger paths

`real/run_pr.sh`: 14 proofs, 14 vacuity twins, 17 reach jobs. Twins and
reach jobs belong here because both depend on the z3 code: an edit can make
a proof pass by making its runs unreachable.

| run | wall | clang | export | `sea-vc` | load |
|---|---|---|---|---|---|
| one job at a time | 321 s | 135 s | 79 s | 94 s (proofs 35, twins 27, reaches 32) | 2.0 / 1.1 |
| four jobs in parallel | 85 s | 140 s | 72 s | 96 s | 8.3 / 9.0 |
| one job at a time, standard headers precompiled (`PM_PCH=1`) | 240 s | 54 s | 76 s | 94 s | 1.1 / 1.1 |

The three middle columns sum the 45 jobs' own times, so in the parallel
run they exceed the wall. The slowest job takes 3.8 s; peak memory of any
process is 0.26 GB. The parallel figure is one measurement. Two thirds of
the sequential time is build and export. With precompiled standard headers
the bitcode of all 45 jobs is identical; the option is off by default
because that identity is checked, not guaranteed by construction. A
compiler cache does not help a first run: the 45 jobs have 45 distinct
sets of `-D` flags.

If the pull request changes the body of a callee, the contract jobs join:
8 rows, 128 s at one job at a time, of which the `remove_element` contract
82 s and its twin 18 s; 1.0 GB peak. These still run on text cut from the
z3 file, not on the real headers.

### Tier B: periodic, or when the harness changes

| what | why | cost | source |
|---|---|---|---|
| the whole pivot gate (`run_gate.sh`): 275 SMT rows with views 1-3 and 97 planted-bug rows, 8 contract rows, the native observer, 8 observer probes, 2 probes that must fail | sensitivity of the gate; validity of the mock | 1,482 s, of which the native observer 141 s | recorded gate log |
| both real-header families with 8 planted-bug rows each (`real/run_real.sh`) | the seam route; planted bugs on the real headers | 53 rows per route; about 7 s per row with build | `real/REAL.md`; Tier A measurement |
| the stored-matrix tableau gate: 63 SMT rows, 203 native exhaustive rows | the other operations of the layer; the step's native exhaustive runs | 3,737 s | recorded gate log |

Tier B needs no trigger from z3. It guards the gate, not the pull request.

### Tier C: once, and on redesign

Non-blank lines, against the 42 of the function: the argument
(`ARGUMENT.md`) 425; the mock (`pm_mock.h`) 283; the harness `main` 137 and
its copy for the real headers 89; the arithmetic lemmas 100; the native
observer 238 and its hooked vector 48; the caller job 169; the real-header
glue (harness, declaration-only `vector.h`, two environment headers, pins)
65 + 106 + 19 + 27; the checks and result classes (`real/check_ir.py`,
`real/strict_patch.py`, `real/gate_result.py`) 317 + 111 + 78.

Open, with no estimate: machine proofs of RC and AGG.

### Maintenance: what an edit to z3 costs the gate

| edit | result on the overlay route | repair |
|---|---|---|
| a new unrelated member, a renamed unrelated function, lines added above the function | pass | none |
| a reformat of the function | pass (Tier B: the 4 planted-bug patches are stale) | 4 patches |
| an unrelated inline member that calls a vector operation the overlay does not declare | stale (clang) | 1 line in the overlay `vector.h` |
| a callee renamed or re-typed | stale (clang) | 1 line, and re-read the contract |
| a mocked member's type changed or the member removed | stale (pin) | re-check the mock |
| the body uses another member of the class | stale (member check) | the argument is extended to that member |
| a callee added to or removed from the body | stale (callee set) | the list in `real/check_ir.py`; a contract if new |
| the body's loops or call pattern changed | violated on `rp-script` or `rp-guarantee` | the argument and the mock are redone |

Of the 21 hypothetical edits examined in `real/REFACTOR.md`, 6 stop the
overlay route although they do not concern the function (5 with the seam, 4
on the text-cutting route), and none leaves it passing while no longer
checking what z3 builds (text cutting: 5).

For scale, from z3's `.github/workflows` at `e8b932052`: `ci.yml` runs on every pull
request with 9 jobs, 8 limited to 90 minutes and one to 10; they build z3
and run `test-z3 -a` and the `z3test` regressions. The repository states
limits, not durations, so no measured figure for z3's CI is given here.

## The gate on z3's own history

The gate was replayed, unchanged, on z3's tree at eight points in time:
the six commits that touched the two headers between June and October
2026, the head, and the parent of the oldest. Where it stopped, the
smallest repair was made and the run repeated. A run is the 45 jobs of Tier
A and 8 planted-bug rows.

| point | what changed for the function | the current gate on that tree |
|---|---|---|
| `e8b932052`, the head | nothing in the two headers | pass, 53 of 53 |
| `8e2087eff` (#11306) | `transpose_rows`; not on the body's path | pass, 53 of 53 |
| `f80fc6fe5` (#11256) | new cells appended through a 2-argument `add_new_element` that returns a reference | pass, 53 of 53 |
| `a200baf44` (#11253) | the counter `m_cancelled` and the counted cleanup scan | stale: here the append callee still has 3 arguments |
| `becb99575` (#10115) | the body of `remove_element` | stale: no `m_cancelled` (pin) |
| `99a8a922e` (#9892), `a5495b78f` (#9783), its parent | annotations; the return type `bool` becomes `void` | stale: no `m_cancelled` (pin) |

The two October changes, read as pull requests under the gate of the point
before them:

**#11256 (`f80fc6fe5`): the append.** The gate stops as stale, at a clang
error inside z3's new 2-argument callee. The repair is one harness line
(the contract for the new signature) and three entries of the callee list.
With it the run is 53 of 53.

**#11253 (`a200baf44`): the counter.** The gate for the code before this
commit is the argument without the counter. It passes 46 of 46 on the
commit's parent, `fcb8c7dfc`. On the commit:

```text
GATE STALE (exit 2): 46 stale; the code changed in a way the harness or the argument does not cover: re-check the argument
  first stale reason (rp-effect-U): STALE: the function now uses member m_cancelled, which the argument
  does not mention (check_ir.py MEMBERS: m_columns, m_rows, m_work_vector_of_row_offsets)
```

No solver runs. Before the member check existed, the same run built and
exited as violated: 35 of 46 rows as expected, four proof jobs `sat`, four
reach jobs `unsat`, and a counterexample in which a zero cell stays in the
row. That counterexample is not a run of z3's code. In the earlier
argument, the iterations of the row loop that the mock does not show
cancel cells and write nothing else.
In the new code each of them also increments the shared counter, and the
cleanup loop stops on that counter. The earlier mock leaves the counter
alone, so its cleanup loop stops too soon.

The correct reading: the earlier argument was sound for the earlier code
(46 of 46), and the current argument holds for this commit's body (53 of
53 with the 3-argument append contract). The change made the earlier
argument no longer fit the code. It is not a z3 defect. A pull request of
this kind has to bring the counter's part of the argument: the counter
equals the number of zero cells at every call the body makes; new cells
are nonzero, which is what allows the scan to start at the old size; cells
above the scan index are nonzero, which is what allows it to stop at 0. In
this harness that part is 58 lines of the mock, one more proof job
(`rp-aggregate`), six arithmetic jobs, and about 125 lines of the written
argument.

Counted over the replay:

| outcome | events | cost |
|---|---|---|
| stop, and the argument depends on what changed | 2: the append callee; the counter | 1 line and 3 list entries; a new part of the argument |
| stop for an unrelated reason | 2: a list of static initializers that a commit outside the two headers changed; one planted-bug patch's context line | 1 line; 1 patch |
| nothing triggered | 1: `becb99575` changed the body of `remove_element` | the callee contracts must be run by path trigger (Tier A, callee part) |
| pass, and nothing the argument uses changed | 2, and the head | none |

## What z3 would need to change

**Nothing**, for the overlay route. It runs on the unmodified tree.

**Optional:** a seam of 2 files, +25 / -12 lines
(`real/refactor-proposal.patch`). It names the class's four container types
in one traits struct, `static_matrix_containers<T, X>`, whose defaults are
today's types. Production code is unchanged: `static_matrix.cpp` at `-O2`
has the same 243 symbols and a byte-identical `.text`. It buys two things:
z3's own `util/vector.h` is no longer shadowed, and an unrelated member
that uses a vector no longer makes the gate stale.

**Names the gate depends on** (a change to one of these is stale, by
design): the signatures of `pivot_row_to_row_given_cell`, `remove_element`,
`add_new_element(unsigned, unsigned)` and `scan_row_strip_to_work_vector`;
the public members `m_rows`, `m_columns`, `m_work_vector_of_row_offsets`
and `m_cancelled` with their current types; the default constructor;
`row_cell<T>` with `var()`, `coeff()`, `offset()`; the helpers `get_val`,
`row_count`, `column_count`, `is_zero`, `addmul`, `mul`; and the container
operations `size`, `empty`, `operator[]` and a range-for over a row.

## Limits and open items

- **Shadowed headers are not watched.** A change to z3's `vector` or
  `std_vector` themselves is not seen; the seam removes this.
- **The view.** With one old cell shown instead of three, a planted bug
  (the cleanup loop scanning upward) passes. Nothing shows three suffices.
- **An equivalent rewrite can be reported as violated.** `rp-script` pins
  the call pattern; reading `rowii.size()` twice is flagged.
- **A callee-body change triggers nothing by itself.** The contract jobs
  must be started by the path trigger, which is not built.
- **Type pins see types, not meanings**, such as a different sentinel
  than -1. Of the 8 planted bugs, 4 are ported to the real headers.
- **Not built:** the CI workflow and path triggers; callee contracts on the
  real headers.

## Commands

In the harness directory:

```sh
export Z3_SRC=<z3 checkout>           # read-only; nothing is built or written there
export SEA=<SeaHorn install prefix>  SEA_MLIR=<directory containing sea-vc>

real/run_pr.sh                        # Tier A; OUT=<dir>, J=4 for four parallel jobs, PM_PCH=1 to precompile
python3 -I real/sweep_real.py rp-guarantee -j 1 --out OUT --show   # one job, with its counterexample if sat
python3 -I ../sweep.py -j 1 --out OUT t23-remove-c4 t23-add2 t23-scan   # callee contracts (tableau gate's sweep)
./run_gate.sh                         # Tier B: planted bugs, views 1-3, contracts, native observer
real/run_real.sh                      # Tier B: both real-header routes with their planted bugs
history/replay.sh                     # the gate on z3's history (needs a z3 clone: Z3_GIT)
```

Each job leaves `build.log` (the exact commands), the compiled copy of
`static_matrix_def.h`, the exported program and the solver's output in its
directory under `OUT`.
