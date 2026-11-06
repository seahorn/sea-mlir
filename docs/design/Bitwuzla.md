# Bitwuzla backend

`--solver=bitwuzla` in `sea-vc` and `sea-expr-eval`; Bitwuzla 0.9.1, C++ API
(`TermManager` + `Bitwuzla`). Optional: configure with
`-DBITWUZLA_ROOT=<install prefix>` (defines `WITH_BITWUZLA`; the library
directory goes into the RPATH of the tools). Only that prefix is searched (no
system Bitwuzla; found again at every configure) and it needs the
`TermManager` API (0.5 or later). Without it `--solver=bitwuzla` is an error
("built without Bitwuzla"), never a fallback to another solver.

Files: `include/sea/Expr/Smt/{MarshalBitwuzla,BitwuzlaSolverImpl,
BitwuzlaModelImpl}.hh`, `lib/Expr/{MarshalBitwuzla,BitwuzlaSolverImpl,
BitwuzlaModelImpl}.cpp`, mirroring the Yices backend.

## Mapping notes

- Covers what `MarshalYices` covers on Bool, bit-vectors and arrays, plus
  uninterpreted functions (`APPLY`) and the mul overflow predicates.
  Integers, reals, quantifiers, lambdas and `ARRAY_DEFAULT` are rejected
  with an error (`sea::diag`, kind `bitwuzla.encode`); the formula is not
  asserted and `Solver::add` returns false (sea-vc exits 1).
- Bitwuzla reports errors by exceptions, which LLVM builds disable: the
  three Bitwuzla sources are compiled with `-fexceptions -frtti` and catch
  everything at the solver / model boundary. No exception escapes.
- A Bitwuzla constant is a fresh term per `mk_const`: the Expr -> Term cache
  is shared by solver and model, and survives `reset()` (new `Bitwuzla`
  instance, same `TermManager`).
- n-ary `and/or/xor/bvadd/bvmul/bvand/bvor/bvxor/concat` are left folds;
  other operators of arity > 2 are rejected. Extension by 0 bits is the
  identity (Bitwuzla rejects it). Shift operands must have equal widths
  (SMT-LIB); Expr's type checker already enforces it.
- Mul overflow is native, like z3: `bvumulo` / `bvsmulo`; z3's separate
  no-overflow / no-underflow predicates (which `expr::op::bv::mulOverflows`
  builds) are `bvsmulo` split by whether the operand signs agree. Checked
  against z3's own evaluation by `check-semantics.py --bitwuzla` over all
  widths / edge values, so `sea-vc` passes `nativeMulOverflow` for it.
- Models: `get_value` always completes (unconstrained constants are 0 /
  false / const-array of 0), which is z3's `complete = true`; the flag is
  ignored. Array values are `CONST_ARRAY` under `ARRAY_STORE`s and decode
  to `constArray` + `store` chain, like Yices. `print` writes one SMT-LIB
  `define-fun` per constant of the asserted formulas of the current context
  (the API has no model printer).
- `unsat_core` is a subset of the assumptions of `check_with_assumptions`
  (`PRODUCE_UNSAT_ASSUMPTIONS`), as for Yices; elsewhere it is an error.
- `to_smt_lib` is `print_formula` (SMT-LIB 2).

## Const-array equalities

Not everything natural is native. Bitwuzla 0.9 answers `unknown` ("equality
over constant arrays not fully supported yet") when an equality reaches a
const-array, and the VC produces them: a memory named at a join is defined
per incoming edge, `g_i -> (m = t_i)`, and `t_i` may be a `sea.mem.const`
under stores. So the backend does inline those definitions
(`ConstArrayDefs`, in `ConstArrayDefs.{hh,cpp}`, shared with Yices; Yices'
select rewriting is not needed). The model evaluates an inlined constant as
its definition; push / pop restore the definitions (as for Yices).

Dropping an equation is only sound in some shapes, so the helper is careful:
- A constant is defined only if its equation is the first thing asserted
  about it (not in an earlier `add` or an outer scope: that formula would keep
  it free) and it does not occur in its own definition. Otherwise the equation
  stays.
- A join, `g_i -> (m = t_i)`, becomes `m = ite(g_1, t_1, ... ite(g_n, t_n, m'))`
  with a fresh free `m'`: `m` stays free when no guard holds. If two guards
  hold, the later ones are ignored, so the definition is exact only if the
  guards are exclusive (they are in the VC: one edge into a join is taken).
  After a `sat` the solver evaluates the guards in the model and answers
  `unknown` (with a warning) if two hold. `unsat` needs no check: the
  definition only weakens.
An array equality with a const-array of another shape (asserted directly)
still gives `unknown` plus Bitwuzla's own warning, and sea-vc reports
`vc.unknown` (Yices: a fatal error, lambdas).

## Performance

Bitwuzla is slower than Yices on the memory-heavy SeaHorn jobs (verify-c-common
`byte_buf_cat`, `hash_table_foreach`, ...: 5 to 30 times, one timeout). The
queries are plain `QF_ABV` with the VC as one conjunction; splitting it into
one assertion per conjunct (tried) does not change that. It is the solver, not
the encoding: see the deeper dive below.

## Checks

`test/VC/bitwuzla/*`, `test/VC/mem/alias-bitwuzla.mlir`,
`test/Interp/semantics-bitwuzla.test` (op semantics, `--bitwuzla`),
`test/VC/fuzz-bitwuzla.test` (VC vs interpreter, `--bitwuzla`). Run once over
the whole corpus (every `sea-vc` RUN line forced to Bitwuzla): all pass but
`VC/names.mlir`, which checks z3's SMT-LIB text.

## TODO: deeper dive

Not done, to do next: look for Bitwuzla options outside the standard API
use that are SMT-LIB compatible and could help the VCs (e.g. `--bv-solver`
engines `preprop` / `prop` / `bitblast`, `--sat-solver` choices, rewrite
level, abstraction, array handling, `--produce-unsat-cores`, per-check
limits, and logic-specific behaviour such as `QF_ABV`), measure them on the
SeaHorn corpus (`test/SeaHorn`), and decide which defaults the backend should
set through `solver_options`. Today it runs with defaults plus models and
unsat assumptions.
