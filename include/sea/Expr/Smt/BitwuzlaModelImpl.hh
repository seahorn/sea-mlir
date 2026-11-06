// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_BITWUZLA
#pragma once

#include "sea/Expr/Smt/BitwuzlaSolverImpl.hh"
#include "sea/Expr/Smt/Model.hh"

namespace sea {
namespace solver {

/* a view of the solver's current model: no state of its own */
class bitwuzla_model_impl : public Model {

  bitwuzla_solver_impl &d_solver;

  expr::ExprFactory &d_efac;

public:
  bitwuzla_model_impl(bitwuzla_solver_impl &solver, expr::ExprFactory &efac);

  ~bitwuzla_model_impl() = default;

  // Bitwuzla always completes: unconstrained constants read as the default
  expr::Expr eval(expr::Expr expr, bool complete) override;

  /// the value of every constant the solver has seen, as SMT-LIB define-funs
  void print(llvm::raw_ostream &o) const override;
};
} // namespace solver
} // namespace sea
#endif
