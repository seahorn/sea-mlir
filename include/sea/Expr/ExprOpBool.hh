// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Boolean Operators
#pragma once
#include "sea/Expr/ExprApi.hh"
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpCore.hh"
#include "sea/Expr/ExprOpSort.hh"
#include "sea/Expr/TypeChecker.hh"
#include "sea/Expr/TypeCheckerUtils.hh"

namespace expr {
namespace op {
enum class BoolOpKind { TRUE, FALSE, AND, OR, XOR, NEG, IMPL, ITE, IFF };

namespace typeCheck {
namespace boolType {

struct OneOrMore : public TypeCheckBase {
  /// \return BOOL_TY
  /// Possible types of children: BOOL_TY
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return typeCheck::oneOrMore<BOOL_TY, BOOL_TY>(exp, tc);
  }
};

struct Unary : public TypeCheckBase {
  /// \return BOOL_TY
  /// Possible types of children: BOOL_TY
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return typeCheck::unary<BOOL_TY, BOOL_TY>(exp, tc);
  }
};

struct Binary : public TypeCheckBase {
  /// \return BOOL_TY
  /// Possible types of children: BOOL_TY
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return typeCheck::binary<BOOL_TY, BOOL_TY>(exp, tc);
  }
};

struct Nary : public TypeCheckBase {
  /// \return BOOL_TY
  /// Possible types of children: BOOL_TY
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return typeCheck::nary<BOOL_TY, BOOL_TY>(exp, tc);
  }
};

struct ITE : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {

    // ite(a,b,c) : a is bool type, b and c are the same type
    if (exp->arity() == 3 && isOp<BOOL_TY>(tc.typeOf(exp->arg(0))) &&
        (tc.typeOf(exp->arg(1)) == tc.typeOf(exp->arg(2))))
      return tc.typeOf(exp->arg(1));

    return sort::errorTy(exp->efac());
  }
};

struct TrueFalse : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return sort::boolTy(exp->efac());
  }
};

} // namespace boolType
} // namespace typeCheck

// -- Boolean operators
NOP_BASE(BoolOp)

/* operator definitions */
NOP(TRUE, "true", PREFIX, BoolOp, typeCheck::boolType::TrueFalse)
NOP(FALSE, "false", PREFIX, BoolOp, typeCheck::boolType::TrueFalse)
NOP(AND, "&&", INFIX, BoolOp, typeCheck::boolType::Nary)
NOP(OR, "||", INFIX, BoolOp, typeCheck::boolType::Nary)
NOP(XOR, "^", INFIX, BoolOp, typeCheck::boolType::Nary)
NOP(NEG, "!", PREFIX, BoolOp, typeCheck::boolType::Unary)
NOP(IMPL, "->", INFIX, BoolOp, typeCheck::boolType::Binary)
NOP(ITE, "ite", FUNCTIONAL, BoolOp, typeCheck::boolType::ITE)
NOP(IFF, "<->", INFIX, BoolOp, typeCheck::boolType::Binary)

/// Boolean construction helpers: the supported way to build Boolean terms.
///
/// Three levels, kept apart on purpose:
///  - construction: mk<AND> / mk<OR> / mk<ITE> / ... build exactly the node
///    asked for (structural primitives; rules that match term shapes use
///    them);
///  - local simplification: land / lor / lneg / limp / lite below build the
///    same operators but fold the trivial cases listed at each helper. The
///    folds look at the operands only (literal TRUE / FALSE, identical
///    operands); there is no flattening of nested AND / OR, no duplicate
///    elimination beyond two identical operands, no complement detection
///    (x && !x), no canonical ordering. Those belong to explicit
///    normalization (boolop::simplify / norm / gather / nnf, BvRules.hh);
///  - validation: none here. Operands must be non-null Boolean terms of one
///    ExprFactory; a fold may return an operand without looking at the
///    others (lite(true, t, e) is t whatever e is), so a successful build
///    proves nothing about the unused operands. Check sorts with a
///    TypeChecker at a validation boundary.
///
/// The binary and the range forms agree: land(a, b) == land({a, b}, efac),
/// lor likewise. bind::lite (ExprOpBinder.hh) is a different policy (it also
/// pushes an ite of two lambdas into a lambda).
namespace boolop {
/// e1 && e2. Folds: x && x = x; true && x = x; x && true = x;
/// false && x = x && false = false.
inline Expr land(Expr e1, Expr e2) {
  if (e1 == e2)
    return e1;

  if (isOpX<TRUE>(e1))
    return e2;
  if (isOpX<TRUE>(e2))
    return e1;
  if (isOpX<FALSE>(e1) || isOpX<FALSE>(e2))
    return mk<FALSE>(e1->efac());

  return mk<AND>(e1, e2);
}

/// e1 || e2. Folds: x || x = x; false || x = x; x || false = x;
/// true || x = x || true = true.
inline Expr lor(Expr e1, Expr e2) {
  if (e1 == e2)
    return e1;

  if (isOpX<FALSE>(e1))
    return e2;
  if (isOpX<FALSE>(e2))
    return e1;
  if (isOpX<TRUE>(e1) || isOpX<TRUE>(e2))
    return mk<TRUE>(e1->efac());
  return mk<OR>(e1, e2);
}

/// e1 -> e2. Folds: true -> x = x; x -> true = true; false -> x = true;
/// x -> x = true. (x -> false is not folded to !x: it would add a negation.)
inline Expr limp(Expr e1, Expr e2) {
  // TRUE -> x  IS x
  if (isOpX<TRUE>(e1))
    return e2;
  // x -> TRUE  IS TRUE
  if (isOpX<TRUE>(e2))
    return e2;
  // FALSE -> x IS TRUE
  if (isOpX<FALSE>(e1))
    return mk<TRUE>(e1->efac());
  // x -> x IS TRUE
  if (e1 == e2)
    return mk<TRUE>(e1->efac());

  // x -> FALSE is missing since it adds a negation

  return mk<IMPL>(e1, e2);
}

/// ite(c, t, e) of any sort. Folds: ite(true, t, e) = t; ite(false, t, e) =
/// e; ite(c, t, t) = t.
inline Expr lite(Expr c, Expr t, Expr e) {
  if (isOpX<TRUE>(c))
    return t;
  if (isOpX<FALSE>(c))
    return e;
  if (t == e)
    return t;

  return mk<ITE>(c, t, e);
}

/// !e1. Folds: !true = false; !false = true; !!x = x.
inline Expr lneg(Expr e1) {
  if (isOpX<TRUE>(e1))
    return mk<FALSE>(e1->efac());
  if (isOpX<FALSE>(e1))
    return mk<TRUE>(e1->efac());

  if (isOpX<NEG>(e1))
    return e1->left();

  return mk<NEG>(e1);
}

namespace detail {
/// The n-ary AND (Op = AND, Unit = TRUE, Zero = FALSE) or OR (OR, FALSE,
/// TRUE) of the range: Unit operands are dropped, a Zero operand gives Zero,
/// no operand left gives Unit, one gives it, two give the binary helper's
/// result, more give one flat n-ary node (operands in range order).
template <typename Op, typename Unit, typename Zero, typename R, typename Bin>
Expr naryFold(const R &r, ExprFactory &efac, Bin binary) {
  ExprVector res;
  for (const Expr &e : r) {
    if (isOpX<Zero>(e))
      return mk<Zero>(efac);
    if (!isOpX<Unit>(e))
      res.push_back(e);
  }
  if (res.empty())
    return mk<Unit>(efac);
  if (res.size() == 1)
    return res.front();
  if (res.size() == 2)
    return binary(res[0], res[1]);
  return mknary<Op>(res.begin(), res.end());
}
} // namespace detail

/// The conjunction of a range of Boolean terms; the empty range is true.
/// Same folds as the binary land on the operands (see detail::naryFold).
template <typename R>
Expr land(const R &r, ExprFactory &efac) {
  return detail::naryFold<AND, TRUE, FALSE>(
      r, efac, [](Expr a, Expr b) { return land(a, b); });
}

/// The disjunction of a range of Boolean terms; the empty range is false.
/// Same folds as the binary lor on the operands (see detail::naryFold).
template <typename R>
Expr lor(const R &r, ExprFactory &efac) {
  return detail::naryFold<OR, FALSE, TRUE>(
      r, efac, [](Expr a, Expr b) { return lor(a, b); });
}

/// The conjunction of a nonempty range (the factory is taken from its first
/// element). Use land(r, efac) when the range may be empty.
template <typename R>
Expr land(const R &r) {
  assert(std::begin(r) != std::end(r));
  return land(r, (*std::begin(r))->efac());
}

unsigned circSize(Expr e);
unsigned circSize(const ExprVector &vec);

/**
 * Very simple simplifier for Boolean Operators
 */
Expr simplify(Expr exp);

/**
 * Very simple normalizer for AND/OR expressions
 */
Expr norm(Expr exp);

/** Gather binary Boolean operators into n-ary ones. Helps
    readability. Best done after NNF */
Expr gather(Expr exp);

/**
 * Converts to NNF. Assumes the only Boolean operators of exp

 * are AND/OR/NEG.
 */
Expr nnf(Expr exp);

/** Makes an expression pretty for printing */
Expr pp(Expr exp);

} // namespace boolop
} // namespace op
} // namespace expr
