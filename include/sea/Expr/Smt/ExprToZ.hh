// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Z3.hh"

namespace sea {
class ExprToZ {
public:
  /// \brief Convert Expr to z3::ast
  /// \p cache is a bidirectional map that is preserved across calls
  /// \p seen local map for current marshal call (in case it is recursive)
  template <typename C>
  static z3::ast marshal(const expr::Expr &e, z3::context &ctx, C &cache,
                         expr_ast_map &seen);
};

class ZToExpr {
public:
  template <typename C>
  static expr::Expr unmarshal(const z3::ast &z, expr::ExprFactory &efac,
                              C &cache, ast_expr_map &seen);
};

class ZToExprNoRec {
public:
  template <typename C>
  static expr::Expr unmarshal(const z3::ast &z, expr::ExprFactory &efac,
                              C &cache, ast_expr_map &seen);
};

} // namespace sea
