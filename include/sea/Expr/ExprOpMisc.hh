// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "sea/Expr/ExprApi.hh"
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpCore.hh"

namespace expr {

namespace op {
enum class MiscOpKind { NONDET, ASM, TUPLE };
// -- Not yet sorted operators
NOP_BASE(MiscOp)

/** A non-deterministic value */
NOP(NONDET, "nondet", FUNCTIONAL, MiscOp, typeCheck::Any)
/** An assumption */
NOP(ASM, "ASM", ADDRESS, MiscOp, typeCheck::Any)
/** A tuple */
NOP(TUPLE, "tuple", FUNCTIONAL, MiscOp, typeCheck::Any)
} // namespace op
} // namespace expr
