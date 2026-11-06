// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaOps.h - sea dialect operations ------------------------*- C++ -*-===//

#ifndef SEA_DIALECT_SEA_SEAOPS_H
#define SEA_DIALECT_SEA_SEAOPS_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Dialect/Sea/SeaTypes.h"

namespace sea {
/// The side effect of print ops (sea.print*): an output stream, so prints
/// are not dead and keep their order; no program state.
struct PrintResource : public mlir::SideEffects::Resource::Base<PrintResource> {
  llvm::StringRef getName() const final { return "<Print>"; }
};
/// True for sea.print, sea.print.mem, sea.print.scope, sea.print.end: ops
/// that never change a program's meaning (docs/design/Print.md).
bool isPrintOp(mlir::Operation *op);
/// The op after `op` in its block, skipping print ops (they never change a
/// program's shape: an assert is "right before" the halt with prints
/// between them).
mlir::Operation *nextNonPrintOp(mlir::Operation *op);
/// The names a weak print may use for `v` (docs/design/Print.md): the name
/// of its NameLoc (a block argument, or the result of a one-result op), and
/// `nd.<site>` for the result of a nondet op (sea.nondet,
/// sea.struct.nondet, sea.ptr.nondet, sea.mem.nondet, sea.memhavoc).
llvm::SmallVector<std::string, 2> printNamesOf(mlir::Value v);
/// The preferred one of them (the NameLoc's), or none.
std::optional<std::string> printNameOf(mlir::Value v);
} // namespace sea

#define GET_OP_CLASSES
#include "sea/Dialect/Sea/SeaOps.h.inc"

namespace sea {
/// True if `program` uses the abstract memory level (!sea.ptr anywhere):
/// it must be placed (sea-place-static) before concrete tools run it.
bool isAbstract(ProgramOp program);
} // namespace sea

#endif // SEA_DIALECT_SEA_SEAOPS_H
