// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
/// Array definitions that bottom out in a const-array, shared by the
/// backends that cannot (Yices: no lambda in a context) or do not fully
/// (Bitwuzla 0.9: "equality over constant arrays not fully supported", the
/// answer is unknown) handle an equality with a const-array.
/// Moved out of the Yices backend.

#include "sea/Expr/Expr.hh"

#include <functional>
#include <vector>

namespace sea {
namespace solver {

/// true iff the array-term skeleton (stores and ites) has a const-array leaf
bool hasConstArrayLeaf(expr::Expr a);

/// The inlined definitions of a solver (one per context: a solver saves a
/// copy at push and restores it at pop; reset clears it).
///
/// add() splits a formula into conjuncts and removes those that define an
/// array constant through a term with a const-array leaf: `a = t`, or the
/// joins of the VC, `g_i -> (a = t_i)`, which define a as
/// ite(g_1, t_1, ... ite(g_n, t_n, a')) with a fresh free a' (a is free when
/// no guard holds). The definitions are substituted in the remaining
/// conjuncts and in later formulas.
///
/// A definition is only taken if it is sound to drop the equation:
///  - a does not occur in its definition, and
///  - a was not in a formula asserted before (that formula would keep a free
///    a), else the equation stays and the solver sees it (Bitwuzla: unknown).
/// A join definition ignores the guards beyond the first that holds: it is
/// exact iff no two guards of a join hold together (true of the VC: one edge
/// into the join is taken). The solver checks that on a model, see
/// joinsExclusive(): unsat is sound without it, sat is not.
struct ConstArrayDefs {
  /// array constant -> its definition (earlier definitions substituted)
  expr::ExprMap defs;
  /// array constants of the formulas asserted so far (not inlinable any more)
  expr::ExprSet used;
  /// the guards of every inlined join with more than one edge
  std::vector<expr::ExprVector> joins;

  /// \p exp without the definitions it contains, defined constants
  /// substituted. Records the definitions and the constants it uses.
  expr::Expr add(expr::Expr exp);

  /// \p e with the definitions substituted: what the solver knows an inlined
  /// constant as (apply before evaluating in a model)
  expr::Expr apply(expr::Expr e) const;

  bool empty() const { return defs.empty(); }

  /// true iff no join has two guards that \p isTrue (the guards, with the
  /// definitions substituted, are to be evaluated in the model of a sat
  /// answer)
  bool joinsExclusive(const std::function<bool(expr::Expr)> &isTrue) const;
};

} // namespace solver
} // namespace sea
