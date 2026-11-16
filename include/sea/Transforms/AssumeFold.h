// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AssumeFold.h - sea-assume-fold: i1 values fixed by assumes -*- C++ -*-//
//
// The pass (lib/Transforms/AssumeFold.cpp) and the analysis it shares with
// sea-vc's report of the branches it leaves (vc.assumed-branches,
// split.assumed-candidate): which i1 values a sea.assume fixes.
// docs/design/AssumeFold.md.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_TRANSFORMS_ASSUMEFOLD_H
#define SEA_TRANSFORMS_ASSUMEFOLD_H

#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/Pass/Pass.h"

namespace sea {

/// sea-assume-fold (lib/Transforms/AssumeFold.cpp).
std::unique_ptr<mlir::Pass> createAssumeFoldPass();

/// On the sea.program after sea-assume-fold: its counters (a dictionary of
/// i64 attributes, the keys of `vc.assume-fold`).
inline constexpr llvm::StringLiteral kAssumeFoldStatsAttr = "sea.assume_fold";

/// An i1 value that is `truth` at every point `source` properly dominates;
/// `source` is a sea.assume or a sea.assert (execution continues past an
/// assert only if it held: for the code it dominates it is an assume).
struct AssumedValue {
  mlir::Value value;
  bool truth;
  mlir::Operation *source;
};

/// The i1 values each sea.assume and sea.assert of `program` fixes
/// (non-constant ones, in program order of the sources; one entry per
/// source and value). From the condition c (true): backward, `and a, b` true
/// fixes a and b true; `or a, b` false fixes both false; `xor a, k` (k a
/// constant) fixes a; `icmp eq|ne a, k` of an i1 a and a constant fixes a;
/// forward, an i1 user of a fixed value that it decides: `xor v, k`, `icmp
/// eq|ne v, k`, `and v, w` with v false, `or v, w` with v true. Values of
/// unreachable blocks are included; the caller filters.
/// `withAsserts` false: the facts of sea.assume ops only.
void collectAssumedValues(ProgramOp program,
                          llvm::SmallVectorImpl<AssumedValue> &out,
                          bool withAsserts = true);

/// A sea.cond_br or sea.select whose condition is an assumed value
/// (collectAssumedValues, at any position: dominated by its source or not).
struct BranchOnAssumed {
  mlir::Operation *op;
  AssumedValue fact;
};

/// The reachable cond_br / select ops of `program` whose condition is an
/// assumed value (facts of sea.assume ops only: in the program the VC
/// encodes the one assert is last, and sea-merge-asserts left an assume
/// of each merged assert's condition on its continuation): after the lowering,
/// the ones sea-assume-fold could not fold (no source of the fact dominates
/// them). One entry per op (its first fact).
void findBranchesOnAssumed(ProgramOp program,
                           llvm::SmallVectorImpl<BranchOnAssumed> &out);

} // namespace sea

#endif // SEA_TRANSFORMS_ASSUMEFOLD_H
