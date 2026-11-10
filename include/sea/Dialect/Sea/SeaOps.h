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
/// The reach site of a check (an i64 discardable attribute on sea.assume
/// and sea.assert, set before the lowering by sea::tagReachSites,
/// sea/VC/Reach.h): the check's position in the input program. The ops a
/// lowering makes in a check's place keep it: the sea.unreachable of a
/// folded assume of false (the same top of op), and the sea.assume
/// sea-merge-asserts makes of an assert (reached once the assert holds).
inline constexpr llvm::StringLiteral kReachSiteAttr = "sea.reach_site";
/// On a sea.program (a unit attribute; sea-vc --reach-keep-sites): the
/// canonicalizer does not erase a tagged assume or assert of the constant
/// true, it replaces it by a reach-site marker, so that the site stays a
/// reach target. Without it, the lowering is the normal one.
inline constexpr llvm::StringLiteral kKeepReachSitesAttr =
    "sea.keep_reach_sites";
/// On a sea.program (a unit attribute; sea-vc --reach-failures):
/// sea-merge-asserts starts the landing block of a tagged assert with the
/// reach-site marker of its failure (kAssertFailMarker).
inline constexpr llvm::StringLiteral kReachFailuresAttr = "sea.reach_failures";
/// A reach-site marker: `sea.print "<op name>"`, a tag line with the
/// reach site (kReachSiteAttr) of the check of true it replaced (its op
/// name: sea.assume or sea.assert). Prints are inert (the VC encodes no
/// term for them); a marker is a reach target (sea/Interp/ReachTargets.h)
/// at the top of the check it stands for.
bool isReachSiteMarker(mlir::Operation *op);
/// The tag of the reach-site marker of an assert's failure: reached iff an
/// execution reaches the assert with its condition false
/// (sea-split-assert-failures, sea-merge-asserts' landing block).
inline constexpr llvm::StringLiteral kAssertFailMarker = "sea.assert.fail";
/// A reach-site marker for `of` (an op name or kAssertFailMarker) with
/// reach site `site`, at the builder's insertion point.
mlir::Operation *createReachSiteMarker(mlir::OpBuilder &b, mlir::Location loc,
                                       llvm::StringRef of,
                                       mlir::Attribute site);
} // namespace sea

#define GET_OP_CLASSES
#include "sea/Dialect/Sea/SeaOps.h.inc"

namespace sea {
/// True if `program` uses the abstract memory level (!sea.ptr anywhere):
/// it must be placed (sea-place-static) before concrete tools run it.
bool isAbstract(ProgramOp program);
} // namespace sea

#endif // SEA_DIALECT_SEA_SEAOPS_H
