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

#define GET_OP_CLASSES
#include "sea/Dialect/SeaImp/SeaImpOps.h.inc"

#endif // SEA_DIALECT_SEAIMP_SEAIMPOPS_H
