# z3: one tableau pivot step under a gate, by argument

A row update in z3's simplex tableau cancels two coefficients. The cleanup
loop finds a zero cell and removes it. Removal moves the row's last cell into
the freed slot. A loop that scans upward then steps past the slot it has just
refilled, and if the moved cell is the second zero, it stays in the row. z3
scans downward, so the cell that moves in comes from above the scan index,
where every cell is already known to be nonzero. That reason is a loop
invariant. The code does not state it, and an exhaustive test on 2x3 matrices
cannot break it, because a 2x3 step never cancels two cells.

This tutorial follows one function, `static_matrix::pivot_row_to_row_given_cell`
(43 lines, `src/math/lp/static_matrix_def.h:53-95`, z3 commit `e8b932052`),
from a failed attempt to verify it with an SMT solver to a regression gate
that runs in about 23 minutes. The first attempt modeled the whole sparse
matrix and gave one call to the solver; the verification condition passed 8 GB
at the smallest shape. The second attempt started from a written argument for
one arbitrary column, and turned each step of the argument into a small job.
All but one of those jobs take 0.05 to 4.2 s. The gate catches seven planted bugs and the historical bug
fixed in z3 #9783, and it flags an eighth, equivalent, change of the call
pattern.

The result is a QA gate. It is stronger than the tests it sits beside and
weaker than a proof: two proof rules are stated and not machine-proved, and
one bound remains. The section "What the gate rests on" lists every item
still on trust.

This page tells how the gate was built. What a z3 developer sees, the exit
codes and the cost per pull request are in
[z3-pivot-gate.md](z3-pivot-gate.md). The harness and its scripts are not
public yet; file names below refer to it.

## Scope

- **Code under the gate:** the body of `pivot_row_to_row_given_cell` at
  `e8b932052`, compiled from z3's text: 43 lines, 42 of them non-blank.
  With the z3 helpers that are compiled and checked with it (`addmul` and
  `mul`, the cell classes and their accessors, `is_zero`; on the
  real-header route also `get_val`, `row_count`, `column_count`) it is 87
  non-blank lines of z3 source. The compiled function is 343 LLVM
  instructions: 61 loads, 52 branches, 65 calls. One job's program after
  inlining and unrolling is about 6,900 lines. Its three callees
  (`remove_element`, `add_new_element`, `scan_row_strip_to_work_vector`) are
  replaced by contracts, and the contracts are proved separately on a small
  stored matrix.
- **Claims:** for every column of the target row, the coefficient after the
  step is the textbook one; a cell exists exactly when the coefficient is
  nonzero; the work vector is reset; the counter `m_cancelled` ends at 0;
  the function's `SASSERT`s hold.
- **Not bounded:** the matrix shape, every index, the length of the pivot
  row, the counter's value on entry.
- **Bounded:** the target row has at most 4 cells on entry (the "view",
  explained below). Numbers are elements of GF(3), the field with three
  elements, in place of rationals, so the big-number code behind every
  `addmul` and `is_zero` on `mpq` is not exercised at all.

## Tools in one paragraph

A *harness* is a C++ program that calls the code under test on
nondeterministic inputs (`nd_u32()`), restricts them with `KIT_ASSUME`, and
states claims with `KIT_CHECK(class, name, condition)`. clang compiles the
harness, SeaHorn unrolls its loops and exports a loop-free program, and
`sea-vc` turns that program into a verification condition (VC), a formula
that is satisfiable exactly when some input violates an enabled check.
`sea-vc` hands the VC to an SMT solver, here z3 itself. `unsat` means no input
violates a check. `sat` comes with a counterexample that `sea-vc` replays on
the program. A *job* is one harness build with one class of checks enabled;
`sweep.py` lists the jobs with their expected verdicts.

## The function

### What a pivot step does to one row

The tableau is a sparse matrix. Pivoting on column `j` in row `i` eliminates
column `j` from every other row. For one other row `ii`, with `alpha = -a(ii,j)`
and the pivot row already divided so that `a(i,j) = 1`, the step is

    row ii  :=  row ii + alpha * row i

after which `a(ii,j) = 0`. The caller, `pivot_column_tableau`, runs this step
once per cell of column `j` outside the pivot row.

### The representation

Each row is a vector of row cells and each column is a vector of column
cells (`static_matrix.h:20-45`). A row cell holds a column index, an offset
into that column's vector, and the coefficient. A column cell holds a row
index and an offset into that row's vector; it has no coefficient. The two
offsets are cross links:

```text
m_rows[ii]      pos 0              pos 1              pos 2
               (var=4, off=1, c)  (var=7, off=0, c)  (var=2, off=3, c)
                  |      \____________
                  v                   \
m_columns[4]    slot 0             slot 1
               (var=.., off=..)   (var=ii, off=0)     <- points back at row ii, pos 0
```

The matrix invariant used throughout says that the links are mutual
inverses, indices are in range, no row holds a column twice, and no stored
coefficient is zero. Zero coefficients are never stored: a cell whose
coefficient becomes zero must be removed.

Three callees maintain the links:

- `remove_element(row, cell)` is a swap-remove on both sides. In the row, the
  last cell is copied into the removed cell's position and the vector is
  popped; the moved cell's column cell gets its offset rewritten. The column
  side does the same.
- `add_new_element(row, col)` appends a cell with coefficient 0 to the row,
  appends the matching column cell, and returns a reference to the new
  coefficient.
- `scan_row_strip_to_work_vector(row)` writes each cell's position into the
  *work vector* `m_work_vector_of_row_offsets`, an `int` per column:
  `w[var(q)] = q`. Between row operations the work vector is all -1.

### The code

```c++
53  template <typename T, typename X> void static_matrix<T, X>::pivot_row_to_row_given_cell(unsigned i,
54                                                      column_cell & c, unsigned pivot_col) {
55      unsigned ii = c.var();
56      SASSERT(i < row_count() && ii < column_count() && i != ii);
57      T alpha = -get_val(c);
58      SASSERT(!is_zero(alpha));
59      auto & rowii = m_rows[ii];
60      remove_element(rowii, rowii[c.offset()]);
61      scan_row_strip_to_work_vector(rowii);
62      unsigned prev_size_ii = static_cast<unsigned>(rowii.size());
63      // run over the pivot row and update row ii
64      m_cancelled = 0;
65      for (const auto & iv : m_rows[i]) {
66          unsigned j = iv.var();
67          if (j == pivot_col) continue;
68          SASSERT(!is_zero(iv.coeff()));
69          int j_offs = m_work_vector_of_row_offsets[j];
70          if (j_offs == -1)   // it is a new element, its coefficient is computed in place
71              mul(alpha, iv.coeff(), add_new_element(ii, j));
72          else {
73              T& c = rowii[j_offs].coeff();
74              addmul(c, iv.coeff(), alpha);
75              if (is_zero(c))
76                  ++m_cancelled;
77          }
78      }
79      // clean the work vector
80      for (unsigned k = 0; k < prev_size_ii; ++k) {
81          m_work_vector_of_row_offsets[rowii[k].var()] = -1;
82      }
83
84      // remove the cancelled cells: only the cells that existed before this update can be
85      // cancelled, the new cells are non-zero, so the scan is skipped unless a cancellation was seen
86      if (m_cancelled > 0) {
87          for (unsigned k = prev_size_ii; k-- > 0 && m_cancelled > 0;  ) {
88              if (is_zero(rowii[k].coeff())) {
89                  remove_element(rowii, rowii[k]);
90                  --m_cancelled;
91              }
92          }
93      }
94      SASSERT(!rowii.empty());
95  }
```

(The line numbers are z3's; the indentation is reduced to fit.) We name the
program points:

| point | lines | what happens |
|---|---|---|
| B0 | 57 | read the pivot cell `(ii, j)` of row `ii`; `alpha` is minus its coefficient |
| B1 | 60 | remove the pivot cell |
| B2 | 61 | scan row `ii` into the work vector |
| B3 | 62 | remember the row's size: cells below `prev_size_ii` are *old*, later ones *new* |
| L1 | 64-78 | for each cell of row `i`: update the old cell of that column in place, or append a new cell |
| L2 | 80-82 | reset the work vector, old positions only |
| L3 | 86-93 | remove the cancelled cells, scanning the old positions downward |
| B4 | 94 | the row is not empty |

### Why this function

The code is on the solver's hot path and was optimized twice in October 2026.
Commit `a200baf44` (#11253) introduced the counter `m_cancelled`:
L1 counts the cells it cancels, and L3 scans only the old positions and stops
when the counter reaches 0. Before, L3 scanned the whole row. That commit's
message reports pivoting at about 19 % of the run time on its profiled
benchmark and 365 thousand pivots on another. Commit `f80fc6fe5` (#11256,
one day later) changed `add_new_element` to return a reference, so that line 71
computes the product in place. A third commit, `8e2087eff` (#11306), touched `static_matrix.h` but not this function.

Each optimization adds a fact the code relies on and does not state:

- new cells are nonzero, so L3 may skip them;
- the counter equals the number of zero cells, so L3 may stop early;
- the cell swapped into a freed slot comes from above the scan index, so L3
  need not look at a slot twice.

### The kind of bug at stake

The bug fixed by `a5495b78f` (#9783, June 2026) lived one level up, in the
caller's loop. Before the fix:

```c++
while (column.size() > 1) {
    auto & c = column.back();
    SASSERT(c.var() != piv_row_index);
    if(! m_A.pivot_row_to_row_given_cell(piv_row_index, c, j)) {
        return false;
    }
    if (m_touched_rows!= nullptr)
        m_touched_rows->insert(c.var());
}
```

`c` refers to the last cell of column `j`. The step removes exactly that
cell: B1 calls `remove_element`, which pops the column's vector. The read
`c.var()` after the call goes through a reference into a popped slot. The
fix's commit message reports that MSVC's address sanitizer found it as a
container overflow (issue #9781). A release build reads the stale slot,
which still holds the right row, so no test output changes. The fix moves
the `insert` before the call, here and at the same read in `dioph_eq.cpp`.

A bug of this kind is a broken contract between two functions, and it is
silent. The gate's argument has to include the fact "after the step, the
slot of `c` is popped", or it cannot see the bug.

## First attempt: model the matrix, hand one call to the solver

The first gate on this layer stored a whole symbolic matrix (the tableau
gate, one directory above this harness). Its harness draws an arbitrary 2x3 tableau: every cell field, every
size and every link is a symbolic value, constrained only by the matrix
invariant. It runs one operation on z3's code over fixed-capacity vectors and
checks the invariant and a dense reference matrix afterwards.

For operations built on one link-chasing primitive this works. At 2x3,
the job that shows `remove_element` keeps the invariant takes 85 s, and the
basis-heading operations take under 1 s each.

For the pivot step it does not work at any shape. `sea-vc` did not finish
generating the VC:

| job | shape | loads | branches | formula nodes | Ackermann pairs | VC generation |
|---|---|---|---|---|---|---|
| `t23-remove-c2` | 2x3 | 308 | 369 | 0.42 M | 22 k | 4.0 s, 0.18 GB |
| `t23-step-c4` | 2x3 | 289 | 184 | 11.5 M | 7.2 M | memory limit: 152 s, 8.1 GB |
| `t24-step-c4` | 2x4 | 391 | 280 | not finished | | memory limit: 153 s, 8.3 GB |
| `t36-step-c4` | 3x6 | 813 | 724 | not finished | | memory limit: 165 s, 8.3 GB |

*Loads* and *branches* count instructions of the exported program. An
*Ackermann pair* is a pair of memory reads that `sea-vc` must compare,
because the two reads may hit the same address; `sea-vc` removes memory from
the formula by adding one constraint per pair. The step has fewer loads and
branches than the removal and about 330 times as many pairs.

The reason is in the code above. The step removes a cell, then merges a row
through the work vector: a column index read from row `i` selects a
work-vector entry, whose value is an offset into row `ii`, where a store
happens. Then it appends at a column read from memory, resets the work
vector at indices read from the row, and removes cells whose offsets the
merge rewrote. Each load's address depends on values loaded after earlier
stores at symbolic addresses, so every load must be resolved against every
such store.

The same harness can still run natively. Enumerating every pre-state of the
step gives 2,832 runs at 2x3, 2,376,576 at 2x4 and 2,831,040 at 3x4, and
all pass. So the function was tested exhaustively at three small shapes and
proved at none.

The lesson: a faithful model of the data structure and a solver do not make
an argument. The model made the solver rediscover, for every pair of cells,
that they do not interfere. A person who explains why the function is
correct never reasons about pairs of cells. They follow one column and say
why the others cannot disturb it. The second attempt writes that
explanation down first and uses the solver only to check its steps.

## The argument, before any tool

`ARGUMENT.md` holds the full text. This section gives its content.

### The contract

`pivot_row_to_row_given_cell(i, c, j)`: `c` is a cell of column `j` that
names row `ii` and position `p` in it.

Preconditions:

- **P1** the matrix invariant;
- **P2** the work vector is all -1;
- **P3** `i != ii`, and `c` is a cell of column `j`;
- **P4** `a(i,j) = 1` (the caller divides the pivot row first);
- **P5** the matrix has at least as many columns as rows.

P5 is needed for an odd reason: the `SASSERT` at line 56 compares the row
index `ii` with `column_count()`. A tableau has one basic column per row, so
P5 holds there.

Postconditions, for every column `k`:

- **Q1 value:** `a'(ii,k) = 0` if `k = j`, else `a(ii,k) + alpha * a(i,k)`;
- **Q2 representation:** `a'(ii,k) != 0` exactly when row `ii` holds one cell
  of column `k`, at a position below the size, with that coefficient;
- **Q3:** `w[k] = -1`;
- **Q4:** `m_cancelled = 0`;
- **Q5:** row `ii` is not empty (line 94). This needs one more assumption:
  the basic column of row `ii` is a unit column. No invariant function in z3
  states it; without it the row can cancel completely.

### One arbitrary column

Fix one column, called kappa. Kappa has at most one cell in row `ii`; call
it D. It has at most one cell in row `i`; call it B, with coefficient `b`.
The argument follows D, B and the work-vector entry `w[kappa]`, plus the
scalars the code reads: the size of row `ii`, `prev_size_ii`, `m_cancelled`
and `alpha`.

A correct step does one of seven things to kappa:

| case | kappa in row `ii` | kappa in row `i` | what must happen to D |
|---|---|---|---|
| U updated | yes | yes, sum nonzero | coefficient becomes `a + alpha*b`, in place |
| C cancelled | yes | yes, sum zero | counted in L1, removed in L3 |
| M moved | yes, and D is the row's last cell when another cell is removed | any | D moves into the freed slot; coefficient and link kept |
| T untouched | yes | no | nothing |
| N new | no | yes | a cell `alpha*b` is appended |
| P pivot | kappa = `j` | yes (coefficient 1) | D is the pivot cell: removed at B1, never re-added |
| Z absent | no | no | stays absent |

Case M can combine with the others. It is listed because it is where the
swap-remove meets the loop order.

### Callee contracts

The body is checked against contracts, stated by position:

- **C-REM** `remove_element(row, cell at q)`: the size drops by one. If `q`
  was not the last position, position `q` now holds the former last cell,
  with its column and coefficient, and that cell's column cell points at
  `q`. Every other position, and every other row, keeps its content. On the
  column side the removed cell's column pops its last slot. The
  precondition allows a zero coefficient, because L3 calls it on cancelled
  cells.
- **C-ADD** `add_new_element(r, col)`: precondition: `col` is absent from row
  `r`. Row `r` gains the cell `(col, 0)` at the old size; the result refers
  to the new coefficient; after a nonzero write through it the invariant
  holds.
- **C-SCAN** `scan_row_strip_to_work_vector(row)`: for every position `q`,
  `w[var(q)] = q`; other entries are unchanged.

### Loop invariants

- **I1** (L1, after some set of row-`i` cells is processed): D is updated
  exactly if B is in that set; the work vector is unchanged since B2; every
  cell at a position `>= prev_size_ii` is new and nonzero; `m_cancelled` is
  the number of old positions whose coefficient is 0.
- **I2** (L2, before index `k`): the work-vector entries of the cells at
  positions below `k` are -1; the rest are unchanged.
- **I3** (L3, before examining `k`): positions above `k` hold nonzero cells;
  positions `<= k` hold the old cells as L1 left them; `m_cancelled` is the
  number of zero cells at positions `<= k`.

I3 carries the reasoning of the opening paragraph. After a removal at `k`,
the cell moved into `k` was the last one, at a position above `k`, so it is
nonzero and the loop may go on to `k - 1`. At exit no zero cell is left, so
the counter is 0.

### Why the other columns do not interfere

Every step that is not "the iteration on kappa" leaves D and `w[kappa]`
alone, or only moves D:

- An L1 iteration on a cell of another column `v` reads `w[v]`. By C-SCAN
  and the no-duplicate invariant, `w[v]` is `v`'s position or -1, never D's
  position. The iteration writes the coefficient at that position, or
  appends a cell at a new position. L1 never writes the work vector.
- An L2 iteration at an index other than D's position writes `w` of another
  column.
- An L3 iteration at another index, and B1 when kappa is not `j`, removes
  another cell or skips. By C-REM, D keeps its column and coefficient. It
  moves only if it was the last cell, and C-REM fixes its link.

Each of those writes also keeps the invariant of the cell it touches: an
L1 update writes `c0 + alpha*e` and increments the counter exactly when the
result is 0; a new cell gets `alpha*e`, which is nonzero; L3 removes only
zero cells.

### The proof rule, in words

Prove everything for one arbitrary column kappa. Whenever the code touches
a cell of another column, assume that cell satisfies the same per-cell
invariant that is being proved for kappa's cell at that kind of program
point.

This looks circular: the claim for kappa assumes the claim for the other
columns. The order of the calls breaks the circle. Number the calls the
body makes, in program order. At call `n`, the check for kappa's cell comes
before any assumption about the cell the call returns, and it uses only
assumptions made at calls before `n`. If the assumptions hold at all calls
before `n`, the checked run is the real run up to `n`, for every choice of
kappa, so the check at `n` holds for every column. That is exactly the
assumption made at `n` about the other cell. Induction on `n` finishes it.

### The proof rule, precisely

*Setting.* Fix a run of the body from a state satisfying P1-P5. Its
*boundaries* are the calls the body makes into mocked operations
(`m_rows[r]`, `rowii[k]`, `w[v]`, `size()`, `empty()`, `begin()`, `*` and
`++` on row `i`, the three callees) and the exit, numbered 1, 2, ... in
program order. For a column kappa, the *kappa-projection* is the run of the
mocked body with kappa distinguished: a boundary that reaches kappa's cells
returns the tracked ones; any other returns a fresh cell of some column `v`.
At boundary `n` of kind `t` the mock first checks the **guarantee**
`G_t(kappa)` about D, then assumes the **rely** `R_t(v)` about the fresh
cell.

*Rule RC.* Premises:

- (H-pre) the run starts in P1-P5;
- (H-con) every callee boundary satisfies its contract;
- (H-job) for every kappa and `n`: every kappa-projection in which the relies
  at all boundaries before `n` hold satisfies `G_t(kappa)` at `n`, and
  Q1-Q5 for kappa at the exit;
- (H-rg) at every `n`: `G_t(v)` for every column `v`, together with the
  counter facts of the section "The counter as an aggregate", implies
  `R_t(v)` for every `v`;
- (H-skip) the assumptions made about the L1 iterations that a projection
  does not show hold when those iterations satisfy their own guarantees;
- (H-proj) in the kappa-projection, D, B, `w[kappa]` and the shared scalars
  at each boundary equal the real ones, given (H-con) and the relies up to
  that boundary.

Conclusion: every guarantee and every rely holds at every boundary of the
run for every column, and Q1-Q5 hold for every column at the exit.

Side conditions: (S1) at each boundary the checks precede the assumptions;
(S2) a guarantee mentions only kappa's cells, the shared scalars and the
boundary's index; (S3) the sequence of calls does not depend on kappa, so
every projection numbers the boundaries alike; (S4) a projection that hides
the iteration on B proves "D untouched" for the shown iteration, and that
carries over because an iteration on `v` reads `w[v]` and the cell there,
never D's value; (S5) the mock's restrictions restrict the conclusion.

**RC is stated, not machine-proved.** `ARGUMENT.md` gives a proof sketch by
strong induction on `n`. S1-S5 are checked by reading the mock. Proving the
rule in a proof assistant is open work. The SMT jobs discharge (H-job) and
the arithmetic part of (H-rg); (H-con) is three jobs on the stored matrix;
the rest is the rule.

## Mocks at call boundaries

No matrix is stored. The harness holds only the tracked state for kappa:
D, B, `w[kappa]`, the scalars, and bookkeeping (`pm_mock.h`, struct
`pm_state`). The vector types the body uses are replaced by mock classes
whose member functions are the boundaries. Each returns the tracked cell
when the index is kappa's and otherwise a fresh cell `X`, drawn
nondeterministically and assumed to satisfy the invariant of that program
point.

Here is the end of `operator[]` on row `ii` (`pm_mock.h:183-204`). `ph` is
the program phase (B0, L1, L2 or L3), `pm.mc` points at `m_cancelled`, and
`pm_nz()` draws a nonzero number:

```c++
    if (pm.d_in && k == pm.d_pos) {                 // the index is D's position
      if (ph == PH_L1 && !pm.d_upd) { pm.d_upd = true; pm.d_mc0 = *pm.mc; }
      if (ph == PH_L3) KIT_CHECK(PM_C_SCRIPT, "I3: D zero is counted", !pm_dzero() || *pm.mc >= 1);
      return PM_D;                                  // return the tracked cell
    }
    if (pm.x_ok && pm.x_ph == ph && pm.x_pos == k) return PM_X; /* the same cell again */
    pm_drop_x();                                    // check the write to the previous fresh cell
    unsigned v = ph == PH_GET ? pm.j : nd_u32();    // a fresh cell: any column ...
    mpq c = ph == PH_GET ? pm.a_iij : pm_nz();
    KIT_ASSUME(ph == PH_GET || v != pm.kap && v != pm.j); /* no duplicate; j removed at B1 */
    if (ph == PH_L2) c = mpq::of((unsigned)ndr(0, LP_P - 1)); /* updated or not: unread */
    if (ph == PH_L3) { /* I3: z zeros left among the n other positions <= k */
      bool dz = pm_dzero() && pm.d_pos < k;
      unsigned z = *pm.mc - (unsigned)dz, n = k + 1 - (unsigned)(pm.d_in && pm.d_pos < k);
      KIT_CHECK(PM_C_SCRIPT, "I3: 0 <= zeros left <= positions left", *pm.mc >= (unsigned)dz && z <= n);
      bool zero = nd_bool();
      KIT_ASSUME(zero ? z >= 1 : z + 1 <= n);       // the rely: I3 for the cell at k
      if (zero) c = mpq();
    }
    new (pm.X_) pm_cell(v, nd_u32(), c);
    pm.x_ok = true; pm.x_new = false; pm.x_ph = ph; pm.x_pos = k; pm.x_c0 = c; pm.x_mc0 = *pm.mc;
    return PM_X;
```

(The `//` comments are added here; the `/* */` ones are the file's.) The
function begins with the guarantees for D at this boundary, each a
`KIT_CHECK` placed before the assumptions, as S1 requires. For example, in
L3:

```c++
    if (ph == PH_L3 && pm.d_in && pm.d_pos > k) /* I3: positions > k are nonzero */
      KIT_CHECK(PM_C_GA, "G-I3a: D above the L3 index is nonzero", !pm_dzero());
```

A fresh cell is *dropped* at the next access elsewhere. `pm_drop_x` then
checks what the body wrote to it: in L1, that the coefficient is
`c0 + alpha*e` and the counter moved exactly if the result is zero. That is
the interference argument's last paragraph, as a check.

The three callees are their contracts, applied to the tracked state. C-REM,
apart from its checks, is three steps: remove D if it is the argument, move
D to the freed position if D was the last cell, decrement the size.

The harness `main` draws the pre-state (`pivot_mock.cc:57-89`, shortened):

```c++
  M m;
  pm.mc = &m.m_cancelled;
  m.m_cancelled = nd_u32(); /* whatever the last row operation left (the body sets 0) */
  /* the call: i != ii rows, A-SHAPE, j the pivot column, kappa arbitrary (32-bit, free) */
  pm.i = nd_u32(); pm.ii = nd_u32(); pm.nrows = nd_u32(); pm.ncols = nd_u32(); pm.j = nd_u32(); pm.kap = nd_u32();
  KIT_ASSUME(pm.i != pm.ii && pm.i < pm.nrows && pm.ii < pm.nrows && pm.nrows <= pm.ncols);
  /* row ii in the view: S0 cells, the pivot cell (ii, j) at p, a(ii,j) != 0 */
  unsigned S0 = (unsigned)ndr(1, PM_VIEW + 1);
  pm.S = S0;
  pm.p = (unsigned)ndr(0, S0 - 1);
  pm.a_iij = pm_nz();
  pm.alpha = -pm.a_iij;
  bool kj = pm.kap == pm.j;
  /* D: kappa's cell of row ii (kappa = j: the pivot cell itself) */
  pm.d_in0 = kj || nd_bool();
  pm.d_pos = kj ? pm.p : (unsigned)ndr(0, S0 - 1);
  KIT_ASSUME(kj || !pm.d_in0 || pm.d_pos != pm.p);
  ...
  lp::column_cell c(pm.ii, pm.p);
  m.pivot_row_to_row_given_cell(pm.i, c, pm.j);
```

Row and column counts, both row indices, the pivot column and kappa are free
32-bit values. `M` is `static_matrix<mpq, mpq>`, and the function called on
the last line is z3's text. After the call, `main` computes the textbook
value `want` for kappa and checks Q1-Q3 against what the mock tracked.

### How the loops are cut

**L1, the loop over row `i`,** is cut by the mock. `begin()` applies a
*skip*: a summary of the iterations before the shown one, which adds `n`
new cells to the size (`n <= 2^24`) and `z` cancellations to the counter.
The iterator then shows one cell: B, or a fresh cell of another column.
`++` checks the shown iteration, applies a second skip for the rest of the
row, and returns `end()`. The solver sees one iteration of L1, and the
length of row `i` is unbounded. The skip is invariant I1 used as a summary.

**L2 and L3 are not cut.** They iterate over the local `prev_size_ii`, and
the loop header calls nothing the mock could intercept. The mock can shorten
them only through the size it returns at B3. That size is the **view**:
`PM_VIEW = 3` old cells, so row `ii` has at most 4 cells on entry. Jobs also
run at views 1 and 2. Rows with more old cells are not covered by any job;
for them I2 and I3 rest on the written argument. This is the method's one
remaining bound.

**The unroll bound is checked.** SeaHorn unrolls each loop `view + 1` times,
which keeps `view` iterations and drops longer runs without any trace. A
dropped run would make a proof say less than it seems to. The job
`pm-bound`, exported at bound `view + 2`, checks at every L2 and L3 read
that the index is below `prev_size_ii <= view`. Two reach jobs show that
runs with exactly `view` iterations of L2 and of L3 exist. So no run of the
unmutated body is cut.

## One obligation, one job

`ARGUMENT.md` numbers 23 obligations. Each maps to one job. Times are from
the recorded full run of November 2026 (one job at a time, a loaded shared
machine, 300 s and 8 GB per job); `wall` excludes the export.

| # | obligation | job | verdict | wall s |
|---|---|---|---|---|
| O1 | C-REM on a stored 2x3 matrix | `t23-remove-c4` | unsat | 93.4 |
| O2 | C-ADD, stored 2x3 (and 2x4) | `t23-add2-c1` | unsat | 2.8 (12.3) |
| O3 | C-SCAN, stored 2x3 (and 2x4) | `t23-scan-c1` | unsat | 1.0 (1.6) |
| O4-O10 | the seven cases U, C, M, T, N, P, Z | `pm-effect-U` .. `-Z` | unsat | 1.5 to 2.8 |
| O11 | Q1-Q3 for kappa, all cases | `pm-value` | unsat | 4.1 |
| O12 | interference: D unchanged but for moves; each other write keeps its cell's invariant | `pm-interference` | unsat | 3.3 |
| O13 | the call script: number and order of calls, callee preconditions, no index past the size, Q4 | `pm-script` | unsat | 2.7 |
| O14 | the body's `SASSERT`s, kappa the basic column of row `ii` | `pm-sasserts` | unsat | 1.9 |
| O15-O17 | the mock's assumptions hold on stored matrices | `obs-23`, `obs-24`, `obs-34` (native) | pass | 0.3, 40, 102 |
| O18 | the step's contract in the caller's loop | `cm-current` | unsat | 0.3 |
| O19 | the same on the pre-#9783 caller | `cm-pre5495` | sat | 0.3 |
| O20 | the guarantee for D at every boundary | `pm-guarantee` (views 3, 2, 1) | unsat | 4.2, 2.7, 2.5 |
| O21 | no run cut by the unroll bound | `pm-bound` (views 3, 2, 1) | unsat | 2.2, 1.5, 1.3 |
| O22 | the counter's step obligations for D | `pm-aggregate` (views 3, 2, 1) | unsat | 3.4, 2.2, 1.6 |
| O23 | the counter arithmetic | six `agg-*` jobs | unsat | 0.05 to 0.39 |

O1 at 2x4 is `unknown` at 300 s; the contract for `remove_element` is
proved by SMT at 2x3 only. A native enumeration at 2x3 (1,670,346 runs)
also passes.

One job, as run for this tutorial (`OUT` is the output directory):

```console
$ python3 -I sweep.py pm-effect-C -j 1 --out OUT
pm-effect-C                                  ok  unsat (2.26s)
pm-effect-C~vacuity                          ok  sat (2.06s)
2/2 as expected  [OUT/results.json]
```

`build.log` in the job's directory records the commands: clang, then the
SeaHorn export with `--bound=4`, then

```console
$ sea-vc OUT/pm-effect-C/prog.mlir --lower --seam --solver=z3 --format=jsonl --timeout=300 --mem-limit=8 --seam-out-of-object=fallback
...
vc-time: 0.531
vc.ackermann-pairs: 5
seam.premises: assumed accesses=3359 conditions=7
solve-time: 1.207
result: unsat
```

The VC has 5 Ackermann pairs. The stored-matrix job for the same function
had 7.2 million when it ran out of memory. The line `seam.premises:
assumed` reports the premises under which `sea-vc` simplifies memory:
distinct objects do not overlap, no object sits at address 0, address
arithmetic does not wrap. With `--seam-out-of-object=fallback` the run
drops them if it finds an access statically outside its object; no pivot
job does. Here the only memory is the harness's own state; accesses to the
row go through the mock, which checks `k < size` itself.

### Vacuity twins and reach jobs

The mock is full of assumptions. If two of them contradicted each other, no
run would reach the end and every proof would pass. Two controls guard
against that.

- A **vacuity twin** is the same program with every check off and
  `assert(false)` at the end of `main`. It must be `sat`: some run gets
  through all the assumptions. Every proof job has one
  (`pm-effect-C~vacuity` above). In the recorded run all 51 twins are sat.
- A **reach job** asserts the negation of one condition of interest, with
  all checks off, and must be `sat`. There is one per case and one per
  situation the argument singles out: D moved at B1, an old D moved in L3,
  a new D moved in L3, two removals in L3, and so on.

A reach job, with the pre-state the solver chose:

```console
$ python3 -I sweep.py pm-reach-2 -j 1 --out OUT --show
pm-reach-2: sat  (ok)  2.1s
  assert-at: pivot_mock.cc:125:3
  ...
  print  pm.S0     3 : ui32
  print  pm.p      2 : ui32
  print  pm.d_in0  1 : si32
  print  pm.d_pos  0 : ui32
  print  pm.b_in   1 : si32
```

Case C is reachable: row `ii` has 3 cells, the pivot cell is last, D is at
position 0, and B exists.

Reach jobs also record what a smaller bound loses. At view 1, three targets
are `unsat` (an old D moved in L3; two removals in L3; D cancelled together
with another removal). The sweep expects those verdicts, so the loss is
written down instead of hidden. At views 2 and 3 every target is reached.

The recorded full sweep has 275 SMT rows, all as expected: 50 proofs, 51
vacuity twins, 73 reaches (8 of them the recorded `unsat` ones), 97 mutant
rows, and 4 jobs expected `sat` for other reasons.

## Checking the mock's assumptions against the real code

A mock that assumes something false proves nothing. Rule RC says where the
assumptions come from; a second, independent check runs them against the
code.

The **observer** (`observe/pm_observe.h`) is a native build. It uses the
stored-matrix harness: z3's function and the real bodies of the three
callees run on a concrete matrix held in fixed-capacity vectors. The
vectors carry a hook at every operation the body can call: `operator[]`,
`size`, `empty`, `begin`, `++`. For a chosen kappa, each hook re-derives
the mock's tracked state from the real matrix and evaluates, as a check,
every condition the mock assumes at that boundary. For example, at an L3
read of a cell other than D, it evaluates the mock's choice
`zero ? z >= 1 : z + 1 <= n` on the real cell and the real counter.

The observer runs on every pre-state of the stored-matrix gate's exhaustive
step enumeration and every kappa below the column count:

| job | shape | steps x columns | runs | result | time |
|---|---|---|---|---|---|
| `obs-23` | 2x3 | 2,832 x 3 | 8,496 | pass | 0.3 s |
| `obs-24` | 2x4 | 2,376,576 x 4 | 9,506,304 | pass | 40 s |
| `obs-34` | 3x4 | 2,831,040 x 4 | 11,324,160 | pass | 102 s |

Run for this tutorial at 2x3:

```console
$ ONLY=23 ./run_observe.sh OUT/observe
extracts: all byte-exact
obs-23 exhaustive: runs=98805 completed=8496 rejected=90309 failing=0 ... exit=0 (0s)
```

The observer is the link between the mock and the code, and it is bounded:
it covers these three shapes and no others. It is also a second
hand-written file that mirrors the mock. Nothing checks mechanically that
the two agree; the mutant probes below are the evidence that the observer
is not blind.

## The counter as an aggregate

### Why the counter is different

Q1-Q3 are per-cell facts: each speaks about kappa's cell alone. "`m_cancelled`
equals the number of zero cells of row `ii`" speaks about all cells at
once. In a kappa-projection only D is tracked. Yet the mock must answer
questions that depend on the count: when L3 reads a fresh cell at index `k`,
may that cell be zero? The answer is "only if a zero is still uncounted
for", which needs the counter and the count to agree.

The mock makes three assumptions of this kind:

1. a skip adds at most `prev_size_ii` cancellations;
2. at the end of L1, the zeros fit the old cells:
   `m_cancelled - [D zero] <= prev - [D old]`;
3. the L3 choice shown in the excerpt above.

In the first version these were assumed and checked only by the observer.
They are now derived.

### The aggregate rule

The treatment follows the standard one for point-wise aggregates in
compositional proofs of parameterized systems: A. S. Wesley, "Smart
Contract Analysis Through Communication Abstractions", MMath thesis,
University of Waterloo, 2021 (<https://uwspace.uwaterloo.ca/handle/10012/17349>),
§3.2.1 and §6.2.

Take a sequence `x` of elements, an Abelian group, and a function `g` from
elements to the group. The *point-wise aggregate* is the fold
`f(x) = g(x_0) + ... + g(x_{S-1})`. Here `x` is row `ii` in position order
and `g(cell) = 1` if the coefficient is 0, else 0, so `f(x)` is the number
of zero cells. Two facts come from the thesis: `f` does not depend on the
order of `x`, and a point write changes it by the difference of one term,
`f(x[i <- n]) = f(x) - g(x_i) + g(n)`.

**Rule AGG (the ghost equals the fold).** Let `c` be a program variable,
here `m_cancelled`. Cut the run into *macro steps*, the code between two
consecutive boundaries. If

- (AGG-0) `c = f(x)` at a starting point, and
- (AGG-1) every macro step is one of, or a composition of:
  - no write: `x` and `c` unchanged;
  - a point write at `i`: `c' = c - g(x_i) + g(n)`;
  - an append of `n`: `c' = c + g(n)`;
  - a swap-remove at `i`: the last element moves to `i`, and `c' = c - g(x_i)`;
- (AGG-2) no composition is open at a boundary,

then `c = f(x)` at every boundary from the starting point on. The point
write is the thesis's. Append and swap-remove are added here and are
**stated, not proved**.

AGG-1 splits per cell. A macro step writes, appends or removes at most one
cell of row `ii`. So the obligation is: the touched cell changes its
contribution by exactly the counter's change, and every other cell keeps its
contribution. With kappa arbitrary, each half is a check on D, and the
job `pm-aggregate` makes those checks:

| step | obligation for D | check name |
|---|---|---|
| line 64 (`m_cancelled = 0`) | the count is 0: D is nonzero | `A-EQ-0` |
| L1 iteration on B, D old | point write; the counter grows by `[D became zero]` | `G-L1`, `G-RS`, `A-EQ-W` |
| L1 iteration on B, D absent | append of a zero cell composed with a nonzero write: net 0 | `G-new`, `A-EQ-W` |
| L1 iteration on another cell | D's contribution unchanged | `G-other`, `A-EQ-W` |
| L2 step, L3 read without removal | no write | `A-EQ-N` |
| L3 removal of D | swap-remove; D is zero; one decrement | `A-EQ-R`, `G-dec` |
| L3 removal of another cell | D's coefficient unchanged; one decrement per removal | `A-EQ-N`, `A-EQ-N/R` |

### The range restriction

The code relies on the counter twice in a way the plain equality does not
give: L3 scans only the old positions, and it stops when the counter
reaches 0. Both need the count over the whole row to equal the count over a
range.

**A-RANGE.** If every cell outside a range R is nonzero, the count over the
row equals the count over R. The premise is a per-cell fact, so it is a
guarantee for D: in L1, R is the old positions (new cells are nonzero);
in L3 at index `k`, R is `[0, k]` (cells above `k` are nonzero); at the
exit, R is empty (no zero cell is left), which gives Q4.

### One term against the sum

The three count assumptions compare one cell with the count. That needs one
lemma that is not per-cell:

**A-TERM.** For 0/1 terms over `n` positions and one of them `t`:
`[x_t] <= sum <= [x_t] + (n - 1)`.

`agg_lemmas.cc` checks the arithmetic as six small jobs over free 32-bit
values: the induction step of the bound, the split of one term, and the
derivation of each count assumption and of Q4 from A-EQ, A-RANGE and
A-TERM. They take 0.05 to 0.39 s. Three more jobs drop the A-RANGE premise
and are `sat`, which shows that the premise is needed. The base case of the
induction (`0 = 0`) and the split's use of order-insensitivity are by hand.

### A correction

The first statement of the invariant was: `m_cancelled` equals the number of
zero cells at every boundary from line 64 on. On the real code this is
false at two places.

- `add_new_element` returns a reference to a new cell whose coefficient is
  0 until `mul` writes through it (line 71). At the callee's return there is
  one more zero cell than the counter says.
- In L3, `remove_element` returns before `--m_cancelled` (lines 89-90). At
  that return the counter is one ahead.

The observer shows both. With the plain equality checked at the callee
returns, at 2x3:

```console
$ PMO_DEFS=-DPMO_AEQ_STRICT=1 ONLY=23 ./run_observe.sh OUT/strict1
obs-23 exhaustive: runs=98805 completed=8496 rejected=90309 failing=576 ...
  first_fail=A-EQ: m_cancelled = the zero cells of row ii ... exit=1 (0s)
$ PMO_DEFS=-DPMO_AEQ_STRICT=2 ONLY=23 ./run_observe.sh OUT/strict2
obs-23 exhaustive: runs=98805 completed=8496 rejected=90309 failing=3456 ...
```

576 of 8,496 runs fail at the return of `add_new_element`, and 3,456 at the
return of `remove_element` in L3. Each is an open composition in the sense
of AGG-2: the append and its write, or the removal and its decrement, form
one macro step. So "boundary" has to mean a call *entry*. At every call
entry the equality holds on all three shapes. The mock's checks were
already at call entries; the statement was wrong, and the rule now says
which points count. The gate keeps both strict runs as probes that must
fail.

The first of the two exists because of the in-place product of #11256:
before it, the callee wrote the coefficient itself. The window lies inside
one statement, line 71.

## The gate against planted bugs

A gate that passes on correct code has shown little until it fails on
wrong code. Eight mutants of the body are each run against six jobs at
views 3 and 1, against the observer, and against the stored-matrix harness
natively. `sat` means caught. The table shows view 3 unless marked.

| mutant | value | script | guarantee | aggregate | value, view 1 | stored matrix, native 2x3 / 2x4 |
|---|---|---|---|---|---|---|
| `scan-off-by-one`: L3 stops before position 0 | sat | sat | unsat | sat: `G-exit` | sat | sat / sat |
| `no-clean`: L2's reset removed | sat | sat | sat (artefact) | sat (artefact) | sat | sat / sat |
| `cancel-no-incr`: a cancellation is not counted | sat | sat | sat: `G-RS` | sat: `A-EQ-W` | sat | sat / sat |
| `no-skip-pivot`: line 67 removed | sat | unsat | sat: `G-col` | unsat | sat | sat / sat |
| `cleanup-ascending`: L3 scans upward | sat | sat | sat: `G-I3a` | sat: `G-I3a` | **unsat** | **unsat** / sat |
| `clean-new-size`: L2 runs to `rowii.size()` (equivalent) | unsat | sat (false alarm) | unsat | unsat | unsat | unsat / unsat |
| `dec-twice`: L3 decrements twice per removal | sat | sat | sat: `G-dec` | sat: `A-EQ-N/R` | unsat | sat / sat |
| `count-new`: a new cell is counted as cancelled | **unsat** | sat | sat: `G-RS` | sat: `A-EQ-W` | unsat | sat / sat |

Every non-equivalent mutant is caught by at least one SMT job at view 3, in
1 to 4 s. Five rows are worth reading closely.

**Caught by the value claim: `scan-off-by-one`.** The loop condition
`k-- > 1` leaves position 0 unexamined. When the cell there cancels, a zero
cell stays in the row and Q2 fails for its column. Because kappa is
arbitrary, `pm-value` finds it. `pm-guarantee` is `unsat`, correctly: no
guarantee before the exit breaks, only exit claims.

**Seen only through the counter: `count-new`.** Line 71 becomes
`{ mul(...); ++m_cancelled; }`. Every coefficient is still right, so Q1-Q3
hold and the value job passes. Run for this tutorial:

```console
$ python3 -I sweep.py pm-mut-count-new-value -j 1 --out OUT --show
pm-mut-count-new-value: unsat  (ok)  3.84s
$ python3 -I sweep.py pm-mut-count-new-aggregate -j 1 --out OUT --show
pm-mut-count-new-aggregate: sat  (ok)  2.45s
  assert-at: pm_mock.h:107:9  (inlined at Z3/src/math/lp/static_matrix_def.h:65:30 <- pivot_mock.cc:89:5)
  path: pm_mock.h:107:9 <- pm_mock.h:106:31 <- Z3/src/math/lp/static_matrix_def.h:71:82 <- pm_mock.h:288:3 <- pm_mock.h:70:7
```

`pm_mock.h:107` is the check `A-EQ-W`: the shown L1 iteration must move the
counter by the contribution of the one cell it wrote. The path goes through
z3's line 71 and the mock's C-ADD: the iteration appended a nonzero cell
and the counter moved by one. The effect on a run is small: L3 loses its
early exit and the counter is nonzero at the end (Q4). The value claim
cannot see a counter that is wrong while every coefficient is right. The
aggregate obligation names it.

**Needs a view above 1: `cleanup-ascending`.** This is the bug of the
opening paragraph. At view 3 the value job is `sat` in 2.2 s; at view 1 it
is `unsat` (1.6 s), because with one old position the upward and downward
scans coincide. The native exhaustive run on stored 2x3 matrices misses it
too, since a 2x3 step never cancels two cells; the 2x4 run catches it. So
the view bound can lose real bugs, and the smallest shape is not always the
smallest interesting one. One caution about the witness: the mock's L3
assumption is an invariant of the downward scan, so this mutant runs under
an assumption it falsifies. The observer reports that directly (first
failing check at 2x3: "A-RANGE in L3: the cells above the index are
nonzero", 1,728 of 8,496 runs). The SMT witness printed for this
tutorial starts from a row of 3 cells with the pivot cell at position 1
and the tracked cell at position 2. Its path through the cleanup loop is
not printed and was not replayed natively, so it may use the assumption
the mutant falsifies. That the bug is real rests on the native 2x4 run,
which fails with an index past the row's size.

**An equivalent mutant: `clean-new-size`.** L2 runs to `rowii.size()`
instead of `prev_size_ii`. The extra iterations write -1 into entries that
are already -1, so behaviour is unchanged. The value job is `unsat` at
unroll bound 4 and again at bound 8 (2.5 s and 4.0 s when run for this
tutorial), which covers L2 runs of up to 7 iterations; equivalence for
longer rows is by reading. The script job is `sat`. It is right to flag
the change and wrong about what it means: the mock's phase tracking assumes
`size()` is called once and L2 makes exactly `prev_size_ii` reads, and the
mutant changes that pattern. (The counterexample's reported location is
line 123 of the mock, an assignment at the end of the iterator's `++`. In
the exported program the check on the second `size()` call follows that
assignment and is false on every path, so the compiler folded it into an
unconditional failure that kept the assignment's location. The native
observer names the check: "size() once, on row ii, after scan".) The
script job is a change detector for the call pattern. A `sat` there says "the argument no longer matches the code;
re-derive it", and it makes no correctness claim. When the script job
fails, only the value job's verdict means anything.

**The same artefact, less benign: `no-clean`.** With L2's reads gone, the
mock takes L3's reads for L2's, because it tells the phases apart by
counting calls. It then gives the L3 cells no I3 assumption. The witnesses
of the guarantee, aggregate and interference jobs on this mutant are not
runs of the mutant. The value job's `sat` is for the right reason (Q3: the
work vector stays dirty), and the script job flags the pattern. That the
other witnesses are artefacts was established by reading the mutant, not by
replay.

**The historical bug, one level up.** The caller's loop is checked against
the step's contract in a separate harness (`caller_mock.cc`): the step is
replaced by "pops the slot of `c` and overwrites it with a poison value".
On the pre-fix loop:

```console
$ python3 -I sweep.py cm-pre5495 -j 1 --out OUT --show
cm-pre5495: sat  (ok)  0.27s
  assert-at: caller_mock.cc:167:3
  ...
  print  cm_cs0.bad        1 : si32
cm-pre5495-nopoison: unsat  (ok)  0.3s
```

The failing check is "touched rows: ... no popped slot's value": the loop
inserted the poison into `m_touched_rows`. The current loop is `unsat`
(`cm-current`, 0.3 s). The second line is the masking case: if the popped
slot keeps its old content, as a `std::vector` in a release build does, the
job is `unsat`. The stored-matrix gate found this bug natively and by
fuzzing only; its SMT job never got past VC generation. This harness does not cover the
branch of the caller that swaps the pivot row's cell to the front of the
column.

Nine checks are falsified by no mutant (`G-pos`, `G-new`, `G-L1`, `G-w`,
`G-other`, the per-boundary `G-col`, `A-EQ-0`, `A-EQ-N` for D's
coefficient, `A-EQ-R`). For those, only the vacuity twin and the reach jobs
show that the check is not empty.

## Verifying the code itself, not cut text

So far "z3's code" has meant text cut from z3's header between marker
strings and compiled inside a hand-written copy of the class. That leaves
room for the gate to check something other than what z3 builds. Three
routes were built, with the same mock and the same `main`.

**E, extraction.** `kit` cuts the function's text from
`static_matrix_def.h` between two marker strings and includes it in a
*shell*: a class named `static_matrix` with the three members the body
uses and its own `get_val`, `row_count` and `column_count`. A byte check
confirms each cut piece is a substring of the z3 file.

**O, real headers with an overlay.** The harness includes z3's
`static_matrix.h` and `static_matrix_def.h` unmodified. Three environment
headers are shadowed on the include path: `util/vector.h` (declaration-only
vector templates), `util/rational.h` (GF(3)) and `util/debug.h` (`SASSERT`
as a check). The mock enters by explicit specialization only:

```c++
template <> int &vector<int>::operator[](unsigned v) { return pm_wvec{}[v]; }
template <> unsigned std_vector<lp::column_strip>::size() const { return pm.ncols; }
namespace lp {
template <> void static_matrix<mpq, mpq>::remove_element(std_vector<row_cell<mpq>> &row, row_cell<mpq> &elem) {
  pm_remove(row, elem);
}
template <> mpq &static_matrix<mpq, mpq>::add_new_element(unsigned i, unsigned j) { return pm_add(i, j); }
template <> void static_matrix<mpq, mpq>::scan_row_strip_to_work_vector(const row_strip<mpq> &rvals) { pm_scan(rvals); }
}
```

No access control is relaxed. The whole vector header must be shadowed
because z3's `std_vector` is an alias of `std::vector` and an alias cannot
be specialized.

**R, a seam in z3.** A proposed patch (`real/refactor-proposal.patch`,
2 files, +25 / -12, not sent upstream) names the class's four container
types in one traits struct:

```c++
template <typename T, typename X>
struct static_matrix_containers {
    using row_offsets = vector<int>;      // indexed by column: offset in the row being pivoted, or -1
    using row         = row_strip<T>;
    using rows        = std_vector<row>;
    using columns     = std_vector<column_strip>;
};
```

```diff
-    vector<int> m_work_vector_of_row_offsets;
+    typename containers::row_offsets m_work_vector_of_row_offsets;
     indexed_vector<T> m_work_vector;
-    std_vector<row_strip<T>> m_rows;
-    std_vector<column_strip> m_columns;
+    typename containers::rows m_rows;
+    typename containers::columns m_columns;
```

plus `row_t` in the six signatures that took a row. The defaults are
today's types. `static_matrix.cpp` compiled at `-O2` with and without the
patch has the same 243 symbols and a byte-identical `.text`. The harness
specializes the struct for its own instantiation, and no z3 header of the
class or its containers is shadowed:

```c++
template <> struct static_matrix_containers<mpq, mpq> {
  using row_offsets = pm_wvec;          /* the work vector */
  using row = pm_std_vector<pm_cell>;   /* a row strip: row i / row ii proxies */
  using rows = pm_std_vector<row>;      /* m_rows: rows i and ii only */
  struct columns {                      /* m_columns: only its size (column_count(), A-SHAPE) */
    unsigned size() const { return pm.ncols; }
  };
};
```

All three routes give the same verdicts on the same jobs (53 of 53 on O and
on R in the last recorded run). clang's IR for the function is the same on
all three: 343 instructions, 61 loads, 52 branches, 65 calls. On O and R
the real class is constructed, so the exported program also contains the
constructor of a `std::deque`; no verdict or time changes. Run for this
tutorial: `rp-value` (route O) `unsat` in 3.9 s, `rf-value` (route R)
`unsat` in 4.0 s.

### What happens when z3 changes

The routes differ in how they fail. `real/REFACTOR.md` tries or reasons
through 21 upstream edits and sorts each outcome into four classes:
**a** the gate keeps working; **b** it stops, and should, because the edit
touches what the argument relies on; **c** it stops for an irrelevant edit;
**d** it keeps passing while no longer checking the code that z3 builds
(silent drift). A selection:

| upstream edit | E | O | R |
|---|---|---|---|
| a new, unrelated data member | a | a | a |
| an unrelated inline member that calls `fill` on the work vector | a | c: the overlay vector lacks `fill` | a |
| the file is reformatted | c: markers and mutant patterns no longer match | a (mutant patches: c) | a (mutant patches: c) |
| a callee is renamed or its signature changes | b: compile error | b: the specialization no longer matches | b |
| the body's loop structure or call pattern changes | a or b: script and guarantee jobs fail | a or b | a or b |
| `get_val` or `column_count` changes | **d**: the shell has its own copies | a | a |
| `m_work_vector_of_row_offsets` becomes `vector<unsigned>` | **d**: the shell declares the old type | b: `static_assert` | b: `static_assert` |
| a second function matching the begin marker is inserted earlier | **d** possible | a | a |
| the body calls a new callee | b: compile error | b: `stale` (IR check) | b: `stale` |
| the body uses a new function-local `static` | **d** (not tried) | b: `stale` | b: `stale` |
| the function moves to a `.cpp` | c | c | c |

Counts over these 21 hypothetical edits, as recorded (the counts from
z3's real history follow below):

| route | c: stops without good reason | d: silent drift | a or b (the rest, by subtraction) |
|---|---|---|---|
| E, extraction | 4 | 5 | 12 |
| O, overlay | 6 | 0 | 15 |
| R, seam | 5 | 0 | 16 |

Extraction drifts silently on the edits a maintainer would most want
caught: a helper or a member type the body depends on. The overlay removes
the drift and adds friction on unrelated uses of the vector type. The seam
removes that friction as well. Its remaining unjustified stops are heavy
new includes with static initializers, mutant patches after a reformat,
and a definition moved out of the header.

O and R have no silent drift only because of two checks added to the
harness build. Before the checks existed, a new callee, a replaced static
initializer and a second object of a mocked container type each built and
ran without a stop on both routes, and a changed default in the seam was
invisible on R.

**Type pins.** The mock replaces whole types, so a change to a type it
stands for would go unseen. The harness pins each one. A pin first tests
that the member exists, so its own message is the first diagnostic whether
the member was re-typed or removed. The current harness on the z3 tree
just before the counter existed (`fcb8c7dfc`), run for this tutorial:

```text
GATE STALE (exit 2): 2 stale; the code changed in a way the harness or the argument does not cover: re-check the argument
  first stale reason (rp-value): real/pivot_real.cc:51:1: static assertion failed due to requirement
  'pm_pin_m_cancelled<lp::static_matrix<rational, rational>>()': pin m_cancelled: static_matrix::m_cancelled
  is missing or no longer unsigned: re-check the argument (RS, A-EQ, Q4) (clang; build.log)
```

On route R the pins are on the struct's defaults and on the members' use of
it. A pin sees types only. A change of meaning with the type unchanged,
such as a different sentinel than -1, is not seen.

**The callee check.** A body edit that adds a callee can compile cleanly:
the specializations serve any code that touches the mocked containers. In
an experiment the body called a new member
`scan_row_strip_to_work_vector_from(rowii, 0)`. Both routes built; the
value job was `unsat` and the script job `sat`. That reads as a property
violation, yet the harness was what no longer fit. `real/check_ir.py` runs
on clang's output before the export. It requires that the functions the
body calls, followed through z3's helpers, are exactly a listed set of
3 contracts, 15 z3 helpers, 13 mock container operations and 4 number
operations; that no other `static_matrix` member is compiled in; that the
work-vector mock is reached only through the member; and that the
translation unit's static initializers are the four known ones. Recorded
for that experiment:

```text
rp-value  BAD stale  STALE: lp::static_matrix<rational, rational>::pivot_row_to_row_given_cell (or a
  z3 helper it calls) calls an unlisted function: void lp::static_matrix<rational, rational>::
  scan_row_strip_to_work_vector_from<...>(...)
```

**The member check.** The same script reads, from the debug information,
which data members of the class the body and its helpers address, and
requires exactly `m_rows`, `m_columns`, `m_work_vector_of_row_offsets` and
`m_cancelled`. A new member that the function does not use changes
nothing. A member the function starts to use, and the argument does not
mention, is `stale`. The next section shows why this check exists.

**Result classes.** Every job lands in one of five classes, and a run
exits with the class of highest precedence: 0 pass, 1 violated, 2 stale,
3 inconclusive, 4 infrastructure. `violated` (a `sat` on a proof job with a
replayed counterexample, or an `unsat` reach or twin) means the code or the
argument is wrong. `stale` means the gate's contact surface changed: a
clang error on the code under the gate, a pin, the callee or member check,
a mutant patch, a failed export. The gate's owner should look; the z3
change is not thereby wrong. Stale outranks violated, because a harness
that no longer fits makes every verdict of the run meaningless.
[z3-pivot-gate.md](z3-pivot-gate.md) shows the output of each class.

**Mutants as patches.** On these routes a mutant is a patch whose text
must match exactly, with no fuzz. It may apply at an offset only if its
text is unambiguous, and every changed line must lie inside the function's
source range as clang reports it, before and after. So an upstream edit
that only shifts lines above the function leaves the mutants applying, and
a patch can never land in a sibling function. Both rules come from
failures: the extraction route's text patterns matched four lines of the
header for `no-clean`, one of them in this function; and `patch --fuzz=0`
applied three old patches at an offset of 78 lines, in
`pivot_row_to_row_given_cell_with_sign`, and reported them killed.

### What happened when z3 did change

The table of edits above is hypothetical. The real counterpart is a replay:
the gate on the real headers, unchanged, run on z3's tree at eight points
in time. They are the six commits that touched the two headers between June
and October 2026, the head, and the parent of the oldest. Where the gate
stopped, the smallest repair was made and the run repeated
(`history/HISTORY.md`).

| point | what changed for the function | the current gate on that tree |
|---|---|---|
| `e8b932052`, the head | nothing in the two headers | pass, 53 of 53 |
| `8e2087eff` (#11306) | `transpose_rows`; not on the body's path | pass, 53 of 53 |
| `f80fc6fe5` (#11256) | new cells appended through the 2-argument `add_new_element` | pass, 53 of 53 |
| `a200baf44` (#11253) | the counter and the counted cleanup scan | stale: here the append callee still has 3 arguments |
| `becb99575` (#10115) | the body of `remove_element` | stale: no `m_cancelled` |
| `99a8a922e` (#9892), `a5495b78f` (#9783), its parent | annotations; the return type `bool` becomes `void` | stale: no `m_cancelled` |

(53 jobs: the 45 of the per-pull-request tier and 8 mutant rows.)

Read forward, as pull requests under the gate of the point before:

- **#11256, the append.** The gate stops as stale, with a clang error
  inside z3's new 2-argument callee. The repair is one harness line (the
  contract for the new signature) and three entries of the callee list.
- **#11253, the counter.** The gate for the code before it is the argument
  without the counter. It passes 46 of 46 on the commit's parent. On the
  commit, every job is stale before any solver runs, and the reason names
  the cause: "the function now uses member m_cancelled, which the argument
  does not mention". Before the member check existed, this run built, and
  it reported a violation: four proof jobs `sat` and four reach jobs
  `unsat`, with a counterexample in which a zero cell stays in the row.
  That counterexample is not a run of z3's code. In the old argument the
  hidden iterations of the row loop cancel cells and touch nothing else. In
  the new code each of them also increments a shared counter, and the
  cleanup loop stops on that counter. The old mock leaves the counter
  alone, so its loop stops early.

  The right reading: the earlier argument was sound for the earlier code,
  and the current argument holds for this commit's body (53 of 53 with the
  3-argument append contract). The change made the earlier argument no
  longer fit the code. z3 has no defect here. What a pull request of this
  kind has to bring is the counter's part of the argument: the counter
  equals the number of zero cells at every call entry, new cells are
  nonzero, cells above the scan index are nonzero. In this harness that is
  58 lines of the mock, the job `pm-aggregate`, and the arithmetic jobs.

Counted over the replay: two stops with a cause the argument depends on
(the two above); two stops for an unrelated reason (a list of static
initializers that a commit outside the two headers changed, and one mutant
patch's context line), each repaired by one line or one patch; one change
that triggered nothing (`becb99575` changed a callee's body, and the
contract job for that callee is not part of this family); and two passes
on commits that changed nothing the argument uses, plus the head.

## What the gate rests on

This is a QA gate. Its job is to fail when a change to the function breaks
one of the stated claims, and to say which one. Against testing and
fuzzing: a test fixes a matrix; the native exhaustive runs cover every
matrix up to 3x4; the SMT jobs cover one arbitrary column of a matrix of
any shape, with any row `i`, provided the following hold. A recorded fuzz
run of the stored-matrix harness (20 s, 2x4) did not find
`scan-off-by-one`.

- **Rule RC** and its side conditions S1-S5: stated, with a proof sketch;
  not machine-proved.
- **Rule AGG** with its append and swap-remove cases, and its side
  conditions (a macro step touches at most one cell; the hidden L1
  iterations change the counter as the shown one does): stated, not proved.
  The classification of the code into macro steps is by reading.
- **A-TERM's base case and split:** by hand.
- **The view.** Row `ii` has at most 4 cells on entry. For longer rows,
  L2 and L3 rest on I2 and I3 as written. View 1 loses a real bug; nothing
  shows that view 3 loses none.
- **The callee contracts** are proved by SMT on a stored 2x3 matrix (C-ADD
  and C-SCAN also at 2x4; C-REM at 2x4 is `unknown` at 300 s). They are
  not proved for every shape.
- **GF(3) numbers.** Cancellation occurs and there is no ordering; the
  comparison operators are deleted in the mock number type, and the build
  succeeds, so the code never orders numbers. Lost: magnitude and bignums.
  The branch of `remove_element` that swaps big coefficients is not
  exercised. That every claim except the value arithmetic is independent
  of the field is a statement by reading.
- **The observer's shapes.** The mock's assumptions, and the counter
  equality itself, are checked on the real row at 2x3, 2x4 and 3x4 only.
  The caller harness's assumptions are not checked natively at all.
- **The memory premises.** `sea-vc` 0.2.0 simplifies memory under three
  premises: distinct objects do not overlap (A-SEP), no object sits at
  address 0 (A-NULL), address arithmetic does not wrap (A-NOWRAP). Every
  pivot job reports `seam.premises: assumed`. The jobs pass
  `--seam-out-of-object=fallback`: if an access statically outside its
  object is found, the run drops the premises and the verdict stays
  unconditional. The tool's default keeps the premises and reports the
  access, which would make a verdict conditional. No pivot job triggers the
  fallback.
- **Rows other than `ii`** keep their values by the callee contracts, not
  by a job on this body.
- **Limits per skip:** at most 2^24 new cells; 32-bit indices.

### Cost

| item | size (non-blank lines) |
|---|---|
| function under the gate | 43 lines of z3 (42 non-blank; 87 with the checked helpers) |
| `ARGUMENT.md` | 425 |
| `pm_mock.h` (mock) + `pivot_mock.cc` (harness) | 283 + 137 |
| `agg_lemmas.cc` | 100 |
| observer: `pm_observe.h` + hooked vector | 238 + 48 |
| `caller_mock.cc` | 169 |
| `sweep.py` | 179 |
| real-header routes: two harnesses, shared `main`, `check_ir.py` | 65 + 87 + 89 + 317 |

The mock is specific to this function: it encodes I1 and I3. The argument
is ten times the length of the code.

Each SMT job on the body takes 1.3 to 4.2 s and 0.1 to 0.2 GB, and does not
depend on the matrix shape. Cost grows mildly with the view: `pm-value`
solves in 1.0, 1.6 and 2.9 s at views 1, 2 and 3. The slowest job in the
gate is the `remove_element` contract on the stored matrix, 93 s. The last
recorded full gate (275 SMT rows, the three contract jobs, the observer at
three shapes, eight observer mutant probes, two strict probes) took 1,393 s
of wall time, one job at a time, on a loaded shared machine; an earlier
recorded run took 1,128 s. Most of the SMT sweep's time is the export, not
the solver.

### What a z3 developer would keep stable

With the seam in place, the gate's contact surface is:

- the two headers and the signature
  `void pivot_row_to_row_given_cell(unsigned, column_cell &, unsigned)`;
- the three callees' names and signatures, and their behaviour as stated in
  C-REM, C-ADD and C-SCAN;
- `static_matrix_containers` with its four member types and their defaults;
- `unsigned m_cancelled`, public;
- the operations the body applies to the containers: `size`, `empty`,
  `operator[]`, a range-for over a row;
- the call pattern the script job pins: `size()` once, L2 making
  `prev_size_ii` reads in ascending order, L3 reading in descending order.

An edit outside this list should leave the gate passing. An edit inside it
should stop the gate, as `stale` or as a failed script job, and the
argument is then re-derived for the new code. A new member, a renamed
unrelated member or a reordered class does not touch the list.

## Reproduce it

Requirements: clang 18, a SeaHorn install with the `sea-mlir-export`
profile, `sea-vc`, and a z3 checkout at `e8b932052`. Nothing is built or
written in the z3 tree. The recorded full runs used clang 18.1.8, SeaHorn
`eae80a105ae1` and sea-vc `v0.1.0-152-ga311e15f48a3`; the single jobs shown
on this page were run with sea-vc 0.2.0.

```sh
export Z3_SRC=<z3 checkout at e8b932052>
export SEA=<SeaHorn install prefix>  SEA_MLIR=<directory containing sea-vc>
# in the harness directory:

python3 -I sweep.py --list                               # every job with its expected verdict
python3 -I sweep.py pm-effect-C -j 1 --out OUT           # one proof and its vacuity twin
python3 -I sweep.py pm-mut-count-new-aggregate -j 1 --out OUT --show   # a mutant, with its witness
python3 -I sweep.py cm-pre5495 -j 1 --out OUT --show     # the #9783 bug in the caller
python3 -I sweep.py agg- -j 1 --out OUT                  # the counter arithmetic

ONLY=23 ./run_observe.sh OUT/observe                     # the observer at 2x3
PMO_DEFS=-DPMO_AEQ_STRICT=1 ONLY=23 ./run_observe.sh OUT/strict1   # must fail: 576 runs

./run_gate.sh                                            # the whole gate, about 20 to 25 minutes
real/run_pr.sh                                           # the per-pull-request tier; exit code = result class
real/run_real.sh                                         # the same jobs on the two real-header routes
history/replay.sh                                        # the gate on z3's history
```

A name selects every job it is a prefix of: `pm-mut-clean-new-size-value`
also runs `...-value-b8`. Each job leaves `build.log` (the exact commands),
`applied.diff` (the mutant, if any), the exported `prog.mlir` and
`vc.jsonl` in its directory under `OUT`. Times are from single runs on a
shared machine and vary. Witnesses vary with the solver's model; the
verdicts do not.

Further reading: [z3-pivot-gate.md](z3-pivot-gate.md) for the gate as a
z3 developer meets it. In the harness: `ARGUMENT.md` (the argument, with
the full rely / guarantee table), `README.md` (every verdict and the notes
on each mutant), `real/REAL.md` and `real/REFACTOR.md` (the real-header
routes and all 21 edits), `history/HISTORY.md` (the replay over z3's
history), and the tableau gate's own README (the stored-matrix gate).
