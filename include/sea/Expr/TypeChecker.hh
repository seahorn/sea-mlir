// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpSort.hh"

#include <string>

namespace expr {

/**
 * \class TypeChecker infers the sort of an expression and checks that it is
 * well-sorted. It is the one sort-inference authority of Expr: the rule of
 * each operator is its inferType (TypeCheckBase, NOP / terminal traits).
 *
 * Queries:
 *  - typeOf(e): the sort of e, or ERROR_TY when e (or a subterm) is
 *    ill-sorted or has no sort rule. Operator rules use it on their kids.
 *  - check(e): the same query as a Result: the sort, or the failure with the
 *    innermost subterm whose rule failed and why. The checker prints
 *    nothing; a caller that wants a message prints Result::why.
 *
 * A failure is either a sort mismatch the rule proved, or an operator
 * without a sort rule (its rule returned null): Result::unsupported tells
 * them apart. Sort inference is not a scope check: bound variables are
 * typed by the sort they carry (BIND(bvar, sort)), not by their binder.
 *
 * Cost and lifetime: results (sorts and failures) are cached per node for
 * the life of the checker, so one checker should serve a whole phase (a VC,
 * an analysis) and repeated queries cost one lookup. The cache holds Expr
 * references: the checker must be destroyed (or reset()) before the
 * ExprFactory of the terms it has seen. reset() drops the cache, e.g.
 * between phases of a long-lived owner. Mutable nodes are cached like the
 * others; reset() after mutating one. Inference walks the term with an
 * explicit stack: deep terms do not grow the call stack.
 *
 * Adding new operators (general): Override the Operator class's inferType
 * method. It should return the type of the specific operator
 *
 * Adding new Normal operators (NOP, DefOp): The type checking is defined
 * externally. Create a struct that inherits TypeCheckBase (in TypeCheckBase.hh)
 * with an inferType(Expr, TypeChecker&) function and pass this struct to
 * NOP/DefOp. A rule returns the sort, or sort::errorTy() (or fail(), to say
 * why) for an ill-sorted term.
 *
 * Adding new Terminal operators: The type checking is defined internally. Every
 * terminal trait defines its own inferType(Expr, TypeChecker&) function
 *
 */
class TypeChecker {
  class Impl;
  Impl *m_impl;

public:
  /// The outcome of check(e).
  struct Result {
    /// the sort of e; null when e is ill-sorted
    Expr sort;
    /// on failure: the innermost subterm whose rule failed (the first one
    /// met, leftmost-innermost)
    Expr culprit;
    /// on failure: why the culprit failed
    std::string why;
    /// on failure: the culprit's operator has no sort rule (as opposed to a
    /// sort mismatch)
    bool unsupported = false;

    explicit operator bool() const { return static_cast<bool>(sort); }
  };

  TypeChecker();
  ~TypeChecker();
  TypeChecker(const TypeChecker &) = delete;
  TypeChecker &operator=(const TypeChecker &) = delete;

  /**
   * The sort of \p e; ERROR_TY if \p e is ill-sorted (see check() for why).
   */
  Expr typeOf(Expr e);

  /**
   * The sort of \p e, or why it has none.
   */
  Result check(Expr e);

  /**
   * For operator rules: report that \p e is ill-sorted because of \p why and
   * return ERROR_TY (the rule's result). The first reason given for a term
   * is kept.
   */
  Expr fail(Expr e, const std::string &why);

  /// Drops every cached result (and the references to the terms they hold).
  void reset();

  /// The number of cached results (for tests and measurements).
  size_t cacheSize() const;
};

} // namespace expr
