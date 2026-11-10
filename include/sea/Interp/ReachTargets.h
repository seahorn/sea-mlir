// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ReachTargets.h - the reach targets of a program ----------*- C++ -*-===//
//
// The targets of reach coverage (docs/reference/output-formats.md, "Reach"):
// the check sites (sea.assume, sea.assert, and sea.unreachable, which is
// where the lowering leaves an assume of constant false), or every block.
// One list and one id per target for every tool that reports reach: sea-vc
// --reach (through the VC), sea-run --reach-targets (concrete runs) and the
// R_T programs of sea-reach-target.
//
// Ids are stable for one input: the op's file position and the call sites
// it was inlined at (`f.c:3:5@g.c:9:2`), a block's from its first located
// op; a position in the .mlir file itself by the file's basename; an op
// without a position gets `unlocated#<k>` (k in program order); a repeated
// id gets `#<n>`.
//
// An op with the attribute sea.reach_target (kReachTargetAttr: the assert
// of R_T, sea-reach-target) has that id: R_T's own target keeps the id of
// the target it stands for. The other targets of R_T are numbered on R_T
// (`#n`, `unlocated#k`, and the sea.unreachable that replaced each other
// sea.halt), so they need not match the input's.
//
// A reach-site marker (sea.print with a reach site, isReachSiteMarker) is a
// check target too, op `sea.print`: the top of the check of true the
// canonicalizer replaced by it under sea-vc --reach-keep-sites.
//
// The failure of a tagged assert (its marker, kAssertFailMarker) has the
// id of the check of its site plus `.fail`.
//
// A check target has the reach site (kReachSiteAttr) its op carries: the
// input check it stands for, the same in every lowering of one input.
//
// A target T is reached by a run when the run is about to execute T's op
// (a block target: its first op): the top of T, every earlier assume
// having held. What the run does at T or after it does not matter.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_INTERP_REACHTARGETS_H
#define SEA_INTERP_REACHTARGETS_H

#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/IR/Location.h"

#include <string>
#include <vector>

namespace sea {

enum class ReachKind { Off, Checks, Blocks };

/// The id of the target an R_T stands for, on R_T's sea.assert.
inline constexpr llvm::StringLiteral kReachTargetAttr = "sea.reach_target";

struct ReachTarget {
  std::string id;
  /// `sea.assume`, `sea.assert`, `sea.unreachable`; `block` for a block
  std::string opName;
  /// the op (a block: its first op): the target is the top of this op
  mlir::Operation *op = nullptr;
  /// the location reported (`at=`): the op's, a block's first located op's
  mlir::Location loc;
  /// the reach site of the check (kReachSiteAttr, sea-vc --reach tags the
  /// input's checks before the lowering), reported as `site=`; -1: none
  /// (a block, or a check the lowering made)
  int64_t site = -1;
  /// the reach site of the next target in the same block (-1: none, or it
  /// has none); with `trueCheck`, T is reached iff that target is
  int64_t nextSite = -1;
  /// an assume or assert of the constant true: it blocks and checks
  /// nothing (canonicalize erases it), reported as `cond=true`
  bool trueCheck = false;
  /// a reach-site marker (isReachSiteMarker, sea-vc --reach-keep-sites):
  /// the op name of the check of true it replaced, reported as `of=`
  std::string of;
};

/// The targets of `program`, in program order (`kind` is not Off).
std::vector<ReachTarget> collectReachTargets(ProgramOp program, ReachKind kind);

/// The target ops of `targets`, in order (FuzzOptions::reachTargets).
std::vector<mlir::Operation *>
reachTargetOps(llvm::ArrayRef<ReachTarget> targets);

} // namespace sea

#endif // SEA_INTERP_REACHTARGETS_H
