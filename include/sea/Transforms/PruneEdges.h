// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- PruneEdges.h - sea-prune-edges: dead CFG edges ------*- C++ -*-===//
//
// The pass (lib/Transforms/PruneEdges.cpp): CFG edges on which no execution
// reaches a sea.assert or a sea.halt are removed (a cond_br becomes an
// assume of the other side's condition and a br; a block left without an
// edge ends in sea.unreachable). docs/design/PruneEdges.md.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_TRANSFORMS_PRUNEEDGES_H
#define SEA_TRANSFORMS_PRUNEEDGES_H

#include "mlir/Pass/Pass.h"

namespace sea {

/// sea-prune-edges. \p fold: sea-assume-fold's rewrite between rounds;
/// \p canonicalize, \p cse: those passes between rounds. With none of the
/// three the pass makes one round (its analysis already follows chains of
/// edges; the clean-ups expose new constant block arguments).
std::unique_ptr<mlir::Pass> createPruneEdgesPass(bool fold, bool canonicalize,
                                                 bool cse);
std::unique_ptr<mlir::Pass> createPruneEdgesPass();

/// On the sea.program after sea-prune-edges: its counters (a dictionary of
/// i64 attributes, the keys of `vc.prune-edges`), only when it pruned an
/// edge.
inline constexpr llvm::StringLiteral kPruneEdgesStatsAttr = "sea.prune_edges";

} // namespace sea

#endif // SEA_TRANSFORMS_PRUNEEDGES_H
