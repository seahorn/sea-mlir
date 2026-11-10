# Expr (ported from SeaHorn)

SeaHorn's Expr library (hash-consed terms) and its z3 / Yices backends,
ported from SeaHorn `dev18` at 0cb925cd (`include/seahorn/Expr`,
`lib/seahorn/Smt`, `lib/Support/{SeaLog,SeaDebug,BoostException}.cc`;
SeaDebug's `LOG` is replaced by `sea/Support/Log.h`).
Namespace stays `expr` so changes can be ported back.

sea-mlir uses Expr for SMT terms only. Changes from SeaHorn:

- `ExprLlvm.hh` → `ExprAPInt.hh`: no `llvm::Value` / `BasicBlock` /
  `Function` terminals (and their terminal sorts and `TerminalKind`s); keeps
  `raw_ostream` printing and APInt <-> mpz.
- Not ported: `ExprMemMap`, `HexDump` (memory models, later),
  `ExprOpFiniteMap` (SeaHorn's finite-map transform), `ExprAig.hh`,
  `EvalModel.hh`, `Evaluate.hh`, `EvalUtils.hh`, `ExprCache.hh`,
  `Smt/ZExprConverter.hh` (unused in SeaHorn and do not compile there).
- `namespace seahorn` → `namespace sea` in the SMT and support code.
- `std::unary_function` / `std::binary_function` bases removed (empty,
  deprecated).
- `boost::throw_exception` reports the error and aborts instead of exiting
  silently with status 1.
- Yices is optional (`SEA_ENABLE_YICES`, defines `WITH_YICES2`; the
  backend's sources are empty without it).
- New (not in SeaHorn): a Bitwuzla backend (`Smt/*Bitwuzla*`, enabled with
  `BITWUZLA_ROOT` as `WITH_BITWUZLA`; see `docs/design/Bitwuzla.md`), and
  `Smt/ConstArrayDefs.hh`, the const-array definition inlining moved out of
  the Yices backend and shared with it.
- `ExprToZ.def`: a failed conversion to z3 exits after reporting (SeaHorn
  only asserted, so release builds continued with a null term).
- Boolean construction: `boolop::land` / `lor` also take a range and the
  factory (`land(r, efac)`: empty is true); binary `lor(x, x)` is `x`. The
  folds of each helper are documented in `ExprOpBool.hh`.
- Sorts: `TypeChecker` is the only inference (`bind::typeOf` / `sortOf`
  are gone). `bind::declaredSort(c)` reads a constant's declared sort
  (structural). `TypeChecker::check(e)` returns the sort or the failure
  (culprit, why) and the checker prints nothing (no `ERRORBINDER`,
  no `getErrorExp`); inference is iterative; `reset()` drops its cache.
  Function application compares each argument with the declared domain,
  and a lambda is typed `FUNCTIONAL_TY(bound sorts..., body sort)`, the
  form of an `FDECL`'s type (SeaHorn wrapped each bound sort in its own
  `FUNCTIONAL_TY`). A bit-vector sort of width 0 and a narrowing
  `zext` / `sext` are ill-sorted.

`ExprRewriter.hh` / `ExprRewriteRules.hh` (new in sea-mlir, not part of the
SeaHorn dev18 port): the multi-step rewriter of SeaHorn's branch seam-dev14
and a minimal rule set; see the comments at the top of `ExprRewriter.hh`
for what differs from the branch and from z3. Tested by
`unittests/ExprRewriter` (`test/Expr/rewriter.test`).
`BaseRegistry.hh`: the object base constants of a SeaM VC (`obj.<name>`)
with allocation order, size and kind; filled by `SeaOpSem` (`--seam`), read
by the rewriter's rules.
`SeaMRules.hh`: the SeaM rewrite rules (pointer normalization, object-aware
compare, read over store / ite, ite and Boolean rules) as one config
(`SeaMRewriteConfig`) and the per-VC driver `SeaMRewriter` (cache +
counters). Each rule's comment names its thesis figure, its assumptions
(`uses: A-INB`; only the compare of a ROW step uses it) and why its RW_n
suffices. Tests: `unittests/SeaMRules`
(`test/Expr/seam-rules.test`), `test/VC/seam/`.

`Ackermann.hh`: the Ackermann lowering of read-only arrays of a query
(fresh constants per read, functional-consistency constraints, pair
reductions by linear forms) and `AckermannModel`, a `solver::Model` that
rebuilds the arrays from a model of the lowered query
(`docs/design/Ackermann.md`; tests `unittests/Ackermann`).

Many headers are not self-contained (they compile only through
`sea/Expr/Expr.hh`), as in SeaHorn. Include `Expr.hh`.

Checked by `utils/check-semantics.py --sea-expr-eval` (via
`tools/sea-expr-eval`): every bit-vector op built as Expr and solved with z3
and with Yices (and Bitwuzla, `--bitwuzla`) agrees with z3's own SMT-LIB
evaluation.
