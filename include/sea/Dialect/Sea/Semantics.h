// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Semantics.h - concrete semantics of sea ops --------------*- C++ -*-===//
//
// The golden semantics of the pure sea ops: total functions over APInt that
// follow SMT-LIB bit-vector semantics. The interpreter and the op folders
// (SeaOps.cpp, used by sccp / canonicalize) use nothing else to compute
// values, so this file is the one place to check against SMT-LIB. It lives
// in the dialect library so that the folders can call it.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_DIALECT_SEA_SEMANTICS_H
#define SEA_DIALECT_SEA_SEMANTICS_H

#include "mlir/IR/Operation.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/APInt.h"

#include <optional>

namespace sea {

/// SMT-LIB `bvudiv`: x / 0 is all ones.
llvm::APInt bvUDiv(const llvm::APInt &x, const llvm::APInt &y);
/// SMT-LIB `bvurem`: x % 0 is x.
llvm::APInt bvURem(const llvm::APInt &x, const llvm::APInt &y);
/// SMT-LIB `bvsdiv`, defined from `bvudiv` by the signs of the operands.
llvm::APInt bvSDiv(const llvm::APInt &x, const llvm::APInt &y);
/// SMT-LIB `bvsrem`: the sign follows the dividend; x % 0 is x.
llvm::APInt bvSRem(const llvm::APInt &x, const llvm::APInt &y);
/// SMT-LIB shifts: a shift amount of at least the width gives 0 (shl, lshr)
/// or the sign fill (ashr).
llvm::APInt bvShl(const llvm::APInt &x, const llvm::APInt &y);
llvm::APInt bvLShr(const llvm::APInt &x, const llvm::APInt &y);
llvm::APInt bvAShr(const llvm::APInt &x, const llvm::APInt &y);

/// Evaluates a pure value op (`sea.const`, bit-vector ops, `sea.icmp`,
/// `sea.select`, casts, address ops, intrinsics) on concrete operands.
/// Returns std::nullopt if `op` is not one of them.
std::optional<llvm::APInt> evalPureOp(mlir::Operation *op,
                                      llvm::ArrayRef<llvm::APInt> operands);

/// The no-wrap promise (`nsw` / `nuw`) of add / sub / mul / shl that the
/// concrete operands break, if any. The op's value is the wrapped one
/// regardless.
std::optional<llvm::StringRef>
brokenWrapPromise(mlir::Operation *op, llvm::ArrayRef<llvm::APInt> operands);

/// The offset of a `sea.ptr.index` (n bits, modular: sum sext(i_k)*s_k + c)
/// from its index values (docs/design/PtrIndex.md).
llvm::APInt ptrIndexOffset(mlir::Operation *op,
                           llvm::ArrayRef<llvm::APInt> indices);

/// The part of a `sea.ptr.index` promise (P1 products, P2 total offset, P3
/// base + offset; nusw or nuw) that the concrete values break, if any. P3
/// is checked only with the base's value (\p base: an address; an abstract
/// pointer has none). The op's value is the wrapped one regardless.
std::optional<llvm::StringRef>
brokenPtrIndexPromise(mlir::Operation *op, std::optional<llvm::APInt> base,
                      llvm::ArrayRef<llvm::APInt> indices);

/// The trip count of one dimension of a `sea.box.all` (docs/design/BoxAll.md):
/// the number of indices lo, lo + step, ... below ub, unsigned: 0 if
/// ub <=u lo, else (ub - lo - 1) /u step + 1 (no wrap; step >= 1). All
/// three have the index width.
llvm::APInt boxTripCount(const llvm::APInt &lo, const llvm::APInt &step,
                         const llvm::APInt &ub);

} // namespace sea

#endif // SEA_DIALECT_SEA_SEMANTICS_H
