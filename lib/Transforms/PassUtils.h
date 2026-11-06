// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- PassUtils.h - helpers shared by the sea passes -----------*- C++ -*-===//
//
// Checked postconditions: each pass checks its postcondition at the end; a
// violation is a bug in the pass (an internal error), reported as
// `<pass>.postcondition`. Index arithmetic shared by the memory passes.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_TRANSFORMS_PASSUTILS_H
#define SEA_TRANSFORMS_PASSUTILS_H

#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/IR/Builders.h"

namespace sea {

/// The value of `v` if it is computed from constants by pure ops only, by
/// the op folders (which evaluate by evalPureOp); the IR is not changed.
/// In the VC lowering sccp has folded these already; this keeps the passes
/// right when run alone.
std::optional<llvm::APInt> foldConstant(mlir::Value v);

/// The index `base` plus `off` units (an address or a pointer; `off` has
/// the index width).
mlir::Value offsetIndex(mlir::OpBuilder &b, mlir::Location loc,
                        mlir::Value base, mlir::Value off);
/// The index `base` plus the constant `i` units.
mlir::Value offsetIndex(mlir::OpBuilder &b, mlir::Location loc,
                        mlir::Value base, uint64_t i);
/// The offset of a sea.ptr.index as plain arithmetic (n bits, modular):
/// sext(i_1)*s_1 + ... + c (docs/design/PtrIndex.md). No flags.
mlir::Value ptrIndexOffsetValue(mlir::OpBuilder &b, PtrIndexOp p);

/// The first op of `program` (its own entry included, for block arguments)
/// with a result or block argument whose type satisfies `bad`, or null.
mlir::Operation *findValueOfType(ProgramOp program,
                                 llvm::function_ref<bool(mlir::Type)> bad);

/// Reports a violated postcondition of `pass` at `at` and returns failure.
mlir::LogicalResult postconditionFailed(llvm::StringRef pass,
                                        const llvm::Twine &what,
                                        mlir::Operation *at);

} // namespace sea

#endif // SEA_TRANSFORMS_PASSUTILS_H
