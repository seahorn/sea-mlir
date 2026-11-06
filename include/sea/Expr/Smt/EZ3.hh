// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "sea/Expr/Smt/ExprToZ.hh"

namespace sea {
#ifdef SEA_REC_ZEXPR
using EZ3 = ZContext<ExprToZ, ZToExpr>;
#else
using EZ3 = ZContext<ExprToZ, ZToExprNoRec>;
#endif
} // namespace sea
