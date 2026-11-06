// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaTypes.h - sea dialect types ---------------------------*- C++ -*-===//

#ifndef SEA_DIALECT_SEA_SEATYPES_H
#define SEA_DIALECT_SEA_SEATYPES_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Types.h"

#include "sea/Dialect/Sea/SeaEnums.h.inc"

#define GET_TYPEDEF_CLASSES
#include "sea/Dialect/Sea/SeaOpsTypes.h.inc"

namespace sea {
/// Bit width of a bit-vector or address type.
unsigned bitWidthOf(mlir::Type t);
} // namespace sea

#endif // SEA_DIALECT_SEA_SEATYPES_H
