// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Reduce.h - sea test-case reduction -----------------------*- C++ -*-===//
//
// Sea-aware reduction for sea-reduce (tools/sea-reduce, docs/Reducer.md).
// The patterns make a program smaller while keeping it a valid sea.program
// (the verifier: one entry block without arguments, unique nondet sites,
// typed block arguments); they do not keep its meaning: the
// interestingness test decides what a reduction must retain.
//
// Upstream mechanism: the sea dialect gets a DialectReductionPatternInterface
// (registerSeaReductionInterface), whose patterns mlir-reduce's
// `reduction-tree` pass applies to the ops outside the range it keeps; the
// same patterns run as passes (sea-reduce-apply, sea-reduce-cleanup) for
// `opt-reduction-pass` and for tests. Each pattern erases only the op it
// matched (reduction-tree applies them to a precollected op list).
//
//===----------------------------------------------------------------------===//

#ifndef SEA_REDUCE_REDUCE_H
#define SEA_REDUCE_REDUCE_H

#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace mlir {
class DialectRegistry;
} // namespace mlir

namespace sea {

/// The pattern names, in decreasing benefit: what sea-reduce-apply{pattern=}
/// and the docs call them.
///   memory-bypass   store / memset / memmove / memhavoc / reset_modified:
///                   the result replaced by the incoming memory
///   drop-assume     sea.assume erased
///   select-branch   cond_br -> br to one successor (the one not ending in
///                   sea.unreachable; else the true one)
///   load-to-nondet  a load of an integer -> sea.nondet "sea.reduce" [fresh
///                   site]; of a pointer -> sea.ptr.nondet
///   forward-operand a value op's result replaced by its first operand of
///                   the same type (add -> lhs, ptr.add -> ptr, select ->
///                   true value, ...)
///   zero-const      an integer value op with no such operand (icmp, casts,
///                   ...) -> sea.const 0
///   erase-dead      an op whose results are all unused (nondets and memory
///                   updates too, which are not trivially dead to MLIR)
llvm::ArrayRef<llvm::StringRef> reductionPatternNames();

/// Adds the named patterns (all when \p names is empty; none for the name
/// `none`). False if a name is unknown.
bool populateSeaReductionPatterns(mlir::RewritePatternSet &patterns,
                                  llvm::ArrayRef<std::string> names = {});

/// The callee name of the nondets the reducer makes (load-to-nondet).
inline constexpr llvm::StringLiteral kReduceNondetCallee = "sea.reduce";

/// sea-reduce-apply: the named patterns applied to the first `limit` ops
/// they match (0: every op), in program order.
std::unique_ptr<mlir::Pass> createReduceApplyPass();
/// sea-reduce-cleanup: erase dead ops (erase-dead), unreachable blocks and
/// unused block arguments; merge a block into its only predecessor when
/// that one branches to it unconditionally. To a fixpoint.
std::unique_ptr<mlir::Pass> createReduceCleanupPass();

/// Registers sea-reduce-apply and sea-reduce-cleanup.
void registerReducePasses();

/// Attaches the sea DialectReductionPatternInterface (all patterns) to the
/// sea dialect (lib/Reduce/ReductionInterface.cpp; links MLIRReduce).
void registerSeaReductionInterface(mlir::DialectRegistry &registry);

/// sea-reduction-tree: reduction-tree's search with the sea patterns only
/// (lib/Reduce/SeaReductionTree.cpp; links MLIRReduce).
void registerSeaReductionTreePass();

/// The patterns the interface gives reduction-tree (empty: all; the
/// special name `none`: no pattern, op deletion only). Set before the
/// pass pipeline is built (sea-reduce --sea-patterns=).
std::vector<std::string> &reductionTreePatterns();

} // namespace sea

#endif // SEA_REDUCE_REDUCE_H
