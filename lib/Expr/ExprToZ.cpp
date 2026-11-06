// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/ExprToZ.hh"
#include "sea/Expr/Smt/EZ3.hh"
#include "sea/Expr/Smt/ExprToZ.def"

#include "sea/Expr/Smt/Z3.hh"

namespace sea {
template z3::ast ExprToZ::marshal<typename EZ3::expr_cache_type>(
    const expr::Expr &, z3::context &, typename EZ3::expr_cache_type &,
    expr_ast_map &);
}
