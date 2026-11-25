// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "sea/Expr/Expr.hh"
#include "sea/Expr/Smt/Z3.hh"

#include <map>
#include <utility>

namespace sea {
class ExprToZ {
public:
  /// How terms are built: one per z3 context, fixed before its first term
  /// (the context's cache keeps what was built)
  struct Options {
    /// n-ary (arity > 2) bvadd, bvmul, bvand, bvor and bvxor as one n-ary
    /// z3 app, the term z3's SMT-LIB parser builds for (bvadd a b c); off:
    /// a right-nested chain of binary apps (sea-vc --solver-nary-sums)
    bool naryBv = false;
    /// the z3 declaration of each such operator, by (Z3_decl_kind, width)
    std::map<std::pair<int, unsigned>, z3::func_decl> decls;
  };

  /// \brief Convert Expr to z3::ast
  /// \p cache is a bidirectional map that is preserved across calls
  /// \p seen local map for current marshal call (in case it is recursive)
  template <typename C>
  static z3::ast marshal(const expr::Expr &e, z3::context &ctx, C &cache,
                         expr_ast_map &seen, Options &opts);
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
