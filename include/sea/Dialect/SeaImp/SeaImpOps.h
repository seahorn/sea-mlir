// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpOps.h - seaimp dialect operations and types --------*- C++ -*-===//

#ifndef SEA_DIALECT_SEAIMP_SEAIMPOPS_H
#define SEA_DIALECT_SEAIMP_SEAIMPOPS_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/MemorySlotInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/SeaImp/SeaImpDialect.h"

#define GET_TYPEDEF_CLASSES
#include "sea/Dialect/SeaImp/SeaImpOpsTypes.h.inc"

namespace sea::imp {
/// For a `!seaimp.var` holding a `!sea.mem`: its index type, its length
/// type (the integer of the index width) and its unit type (one index unit).
/// For any other type: NoneType (the custom parser then reports a type
/// mismatch; the verifier the variable's type).
mlir::Type regionIndexType(mlir::Type varType);
mlir::Type regionLenType(mlir::Type varType);
mlir::Type regionUnitType(mlir::Type varType);
/// The cell type of the memory a variable holds (NoneType for a variable
/// that holds no memory).
mlir::Type regionCellType(mlir::Type varType);
} // namespace sea::imp

#define GET_OP_CLASSES
#include "sea/Dialect/SeaImp/SeaImpOps.h.inc"

#endif // SEA_DIALECT_SEAIMP_SEAIMPOPS_H
