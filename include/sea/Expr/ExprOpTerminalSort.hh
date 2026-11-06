// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Sorts and types
#pragma once

#include "sea/Expr/ExprApi.hh"
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpCore.hh"
#include "sea/Expr/ExprOpSort.hh"
#include <string>
namespace expr {

namespace op {

enum class TerminalTypeOpKind {
  STRING_TERMINAL_TY,
  BVAR_TERMINAL_TY,
  MLIR_VALUE_TERMINAL_TY,
  MLIR_BLOCK_TERMINAL_TY
};

NOP_BASE(TerminalTypeOp)

/// \brief String terminal type,
NOP(STRING_TERMINAL_TY, "STRING TERMINAL", PREFIX, TerminalTypeOp,
    typeCheck::simpleType::Simple)
/// \brief Bounded variable terminal type,
NOP(BVAR_TERMINAL_TY, "BVAR TERMINAL", PREFIX, TerminalTypeOp,
    typeCheck::simpleType::Simple)
/// \brief mlir::Value terminal type,
NOP(MLIR_VALUE_TERMINAL_TY, "MLIR::VALUE TERMINAL", PREFIX, TerminalTypeOp,
    typeCheck::simpleType::Simple)
/// \brief mlir::Block terminal type,
NOP(MLIR_BLOCK_TERMINAL_TY, "MLIR::BLOCK TERMINAL", PREFIX, TerminalTypeOp,
    typeCheck::simpleType::Simple)
} // namespace op

namespace op {
namespace sort {
inline Expr stringTerminalTy(ExprFactory &efac) {
  return mk<STRING_TERMINAL_TY>(efac);
}
inline Expr bvarTerminalTy(ExprFactory &efac) {
  return mk<BVAR_TERMINAL_TY>(efac);
}
inline Expr mlirValueTerminalTy(ExprFactory &efac) {
  return mk<MLIR_VALUE_TERMINAL_TY>(efac);
}
inline Expr mlirBlockTerminalTy(ExprFactory &efac) {
  return mk<MLIR_BLOCK_TERMINAL_TY>(efac);
}

} // namespace sort
} // namespace op
} // namespace expr
