// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_YICES2
#pragma once

#include "yices.h"

#include "sea/Expr/Smt/Model.hh"
#include "sea/Expr/Smt/Yices2SolverImpl.hh"

namespace sea {
namespace solver {

class yices_model_impl : public Model {

  /* printing defaults */
  static uint32_t width;
  static uint32_t height;
  static uint32_t offset;

  /* the underlying yices model */
  model_t *d_model;

  /* the underlying context */
  yices_solver_impl &d_solver;

  expr::ExprFactory &d_efac;

public:
  yices_model_impl(model_t *model, yices_solver_impl &solver,
                   expr::ExprFactory &efac);

  ~yices_model_impl();

  // yices ignores the complete flag
  expr::Expr eval(expr::Expr expr, bool complete) override;

  void print(llvm::raw_ostream &o) const override;
};
} // namespace solver
} // namespace sea
#endif
