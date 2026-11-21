// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpDialect.h - seaimp dialect -------------------------*- C++ -*-===//

#ifndef SEA_DIALECT_SEAIMP_SEAIMPDIALECT_H
#define SEA_DIALECT_SEAIMP_SEAIMPDIALECT_H

#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/Dialect.h"

#include "sea/Dialect/SeaImp/SeaImpOpsDialect.h.inc"

namespace sea::imp {
/// The first seaimp op under `root` (in walk order), or null. The tools
/// that run sea programs (sea-run, sea-vc without --lower) refuse a
/// program that has one: seaimp has meaning only through its lowering.
mlir::Operation *findSeaImpOp(mlir::Operation *root);
} // namespace sea::imp

#endif // SEA_DIALECT_SEAIMP_SEAIMPDIALECT_H
