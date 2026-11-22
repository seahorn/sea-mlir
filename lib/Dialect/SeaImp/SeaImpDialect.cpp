// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpDialect.cpp - seaimp dialect -----------------------*- C++ -*-===//

#include "sea/Dialect/SeaImp/SeaImpDialect.h"
#include "sea/Dialect/Sea/SeaTypes.h"
#include "sea/Dialect/SeaImp/SeaImpOps.h"

#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace sea::imp;

#include "sea/Dialect/SeaImp/SeaImpOpsDialect.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "sea/Dialect/SeaImp/SeaImpOpsTypes.cpp.inc"

LogicalResult VarType::verify(function_ref<InFlightDiagnostic()> emitError,
                              Type valueType) {
  if (auto i = dyn_cast<IntegerType>(valueType)) {
    if (!i.isSignless())
      return emitError() << "a variable holds a signless integer, got "
                         << valueType;
    return success();
  }
  if (isa<sea::PtrType, sea::StructType, sea::MemType>(valueType))
    return success();
  if (isa<sea::AddrType>(valueType))
    return emitError() << "address variables are not supported (no nondet "
                          "initial value for !sea.addr; use !sea.ptr or an "
                          "integer), got "
                       << valueType;
  return emitError() << "a variable holds a signless integer, a !sea.ptr, "
                        "a !sea.struct or a !sea.mem, got "
                     << valueType;
}

void SeaImpDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "sea/Dialect/SeaImp/SeaImpOps.cpp.inc"
      >();
  addTypes<
#define GET_TYPEDEF_LIST
#include "sea/Dialect/SeaImp/SeaImpOpsTypes.cpp.inc"
      >();
}

Operation *sea::imp::findSeaImpOp(Operation *root) {
  Operation *found = nullptr;
  root->walk([&](Operation *op) {
    if (!isa_and_nonnull<SeaImpDialect>(op->getDialect()))
      return WalkResult::advance();
    found = op;
    return WalkResult::interrupt();
  });
  return found;
}
