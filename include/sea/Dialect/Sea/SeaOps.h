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
#include "llvm/ADT/StringSet.h"

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

/// Stable block ids (sea-vc --reach; utils/cover-diff.py level 1): an
/// array of strings on a block's terminator, the pieces of input blocks the
/// block holds, in path order. sea::tagBlockIds (sea/VC/Reach.h) gives
/// every block of the input its position ("<n>") before any pass; a pass
/// that changes the CFG keeps them: a merge of a block into its only
/// predecessor concatenates the two lists; a split keeps the list on the
/// head and gives the tail a derived id ("<last>.s<k>",
/// BlockIdNamer::splitTail); a new block gets a derived id
/// ("<last>.<role><k>"); a fold that ends a block early keeps its list;
/// an erased block's ids are gone. A derived id is made in the same way in
/// two lowerings of one input (the passes are deterministic), which is
/// what matches the blocks.
/// A program without them is the normal lowering: nothing is added.
/// sea-vc --smt2 prints the ids of each block guard (`; sea.block-ids`),
/// from which query-diff builds the Skolem hints of --project-blocks.
inline constexpr llvm::StringLiteral kBlockIdsAttr = "sea.block_ids";
/// The ids of `b` (on its terminator), or null.
mlir::ArrayAttr blockIdsOf(mlir::Block *b);
/// Puts the ids of `from`'s block on `to` (a terminator), if it has any.
void copyBlockIds(mlir::Block *from, mlir::Operation *to);
/// The ids of a merged block: `a` then `b`, without repeats; null if both
/// are null.
mlir::ArrayAttr concatBlockIds(mlir::ArrayAttr a, mlir::ArrayAttr b);
/// The number of blocks a path from the entry reaches that carry no ids, in
/// a program some block of which carries them (0 otherwise); `first`: the
/// terminator of the first such block.
unsigned countBlocksWithoutIds(ProgramOp program,
                               mlir::Operation **first = nullptr);

/// Fresh derived block ids within one program: `<base>` if no block has it,
/// else `<base>.<k>`, k the least free one.
class BlockIdNamer {
public:
  explicit BlockIdNamer(ProgramOp program);
  /// An id derived from `parent`'s last id (the piece the new block
  /// leaves from) and `role`: `<last>.<role><k>`; null if `parent` has no
  /// ids (the normal lowering).
  mlir::StringAttr derive(mlir::Block *parent, llvm::StringRef role);
  /// An id named `base` (fresh as above), always.
  mlir::StringAttr fresh(llvm::StringRef base);
  /// A split: `tail`, the part of a block after the split point (it holds
  /// the block's terminator), gets the derived id `<last>.s<k>` (one piece
  /// of the block: a path can reach the head and not the tail); returns
  /// the block's ids, for the head (null if it has none).
  mlir::ArrayAttr splitTail(mlir::Block *tail);
  /// Sets `ids` = [id] on terminator `term`, if `id` is not null.
  static void set(mlir::Operation *term, mlir::StringAttr id);

private:
  mlir::MLIRContext *ctx;
  llvm::StringSet<> used;
};
} // namespace sea

#endif // SEA_DIALECT_SEA_SEAOPS_H
