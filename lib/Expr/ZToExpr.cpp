// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/ZToExpr.def"
#include "sea/Expr/Smt/EZ3.hh"
#include "sea/Expr/Smt/ExprToZ.hh"

#include "sea/Expr/Smt/Z3.hh"

namespace sea {
template expr::Expr ZToExpr::unmarshal<typename EZ3::z_cache_type>(
    const z3::ast &, expr::ExprFactory &, typename EZ3::z_cache_type &,
    ast_expr_map &);
}
