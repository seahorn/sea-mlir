// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
/// Overflow predicates encoded with the solver's native operators, shared
/// by the VC generator (sea.umulo / sea.smulo) and sea-expr-eval (so the
/// semantics oracle checks the VC's encoding).

#include "sea/Expr/Expr.hh"

namespace expr {
namespace op {
namespace bv {

/// Bool: the product of a and b overflows (SMT-LIB 2.7 bvumulo / bvsmulo).
/// Signed: z3 splits it into "no overflow" and "no underflow".
inline Expr mulOverflows(bool isSigned, Expr a, Expr b) {
  if (!isSigned)
    return mk<NEG>(mk<UMUL_NO_OVERFLOW>(a, b));
  // two statements: the order of function arguments is unspecified, and the
  // creation order of the nodes decides the printed order of the and
  Expr noOverflow = mk<SMUL_NO_OVERFLOW>(a, b);
  Expr noUnderflow = mk<SMUL_NO_UNDERFLOW>(a, b);
  return mk<NEG>(mk<AND>(noOverflow, noUnderflow));
}

} // namespace bv
} // namespace op
} // namespace expr
