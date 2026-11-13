// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- RangeFold.h - sea-range-fold: folds settled by integer ranges -*- C++
//-*-//
//
// The pass and the pieces of it the exhaustive range test checks
// (unittests/IntRanges): the interval a simple bound `icmp pred x, C` gives
// x, and the step of a bound back through sext / zext.
// docs/design/RangeFold.md.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_TRANSFORMS_RANGEFOLD_H
#define SEA_TRANSFORMS_RANGEFOLD_H

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/Interfaces/InferIntRangeInterface.h"
#include "mlir/Pass/Pass.h"

#include <optional>

namespace sea {

/// sea-range-fold (lib/Transforms/RangeFold.cpp).
std::unique_ptr<mlir::Pass> createRangeFoldPass(RangeFoldMode mode,
                                                unsigned maxDepth = 12);
std::unique_ptr<mlir::Pass> createRangeFoldPass();

/// On the sea.program after sea-range-fold: its counters (a dictionary of
/// i64 attributes, the keys of `vc.range-fold`).
inline constexpr llvm::StringLiteral kRangeFoldStatsAttr = "sea.range_fold";

/// An empty range (no value): unsigned or signed bounds crossed.
bool isEmptyRange(const mlir::ConstantIntRanges &r);
/// `r` with each view (unsigned, signed) narrowed by what the other implies
/// where the two orders agree (an interval inside one sign half is the same
/// interval in both orders); the same values. ConstantIntRanges keeps the
/// two views apart: an intersection narrows each by itself only.
mlir::ConstantIntRanges tightenRange(const mlir::ConstantIntRanges &r);
/// The empty range of `width` bits.
mlir::ConstantIntRanges emptyRange(unsigned width);

/// The values x of c's width with `icmp pred x, c` true, as a range (empty
/// when none). std::nullopt: no interval holds them exactly and none is
/// taken (`ne` of a constant inside the range: a hole).
std::optional<mlir::ConstantIntRanges> boundOfPredicate(ICmpPredicate pred,
                                                        const llvm::APInt &c);
/// The predicate p' with `icmp p a, b` == `icmp p' b, a`.
ICmpPredicate swapPredicate(ICmpPredicate pred);

/// The values y (of `narrowWidth` bits) with ext(y) in `r` (sext if
/// `isSigned`, else zext), as a range containing every such y (empty when
/// none).
mlir::ConstantIntRanges rangeBeforeExt(const mlir::ConstantIntRanges &r,
                                       bool isSigned, unsigned narrowWidth);

} // namespace sea

#endif // SEA_TRANSFORMS_RANGEFOLD_H
