// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SplitCriticalEdges.cpp - BMC form: no critical edges -----*- C++ -*-===//
//
// An edge is critical if its source has several successors and its target
// several predecessors (counting edges, so a cond_br with both targets equal
// makes both edges critical). Each one gets a new block that only branches,
// with the edge's block arguments, to the target. Upstream MLIR has no such
// pass.
//
// Contract.
//   Accepts: any CFG of sea.br / sea.cond_br (the only terminators with
//     several successors).
//   Produces: no critical edge, and so no cond_br with equal targets
//     (checked). New blocks only branch; values are unchanged.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class SplitCriticalEdgesPass
    : public PassWrapper<SplitCriticalEdgesPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitCriticalEdgesPass)

  StringRef getArgument() const final { return "sea-split-critical-edges"; }
  StringRef getDescription() const final {
    return "Put a block on every critical edge (BMC form)";
  }

  void runOnOperation() override;
};

static unsigned numPredecessorEdges(Block *b) {
  return std::distance(b->pred_begin(), b->pred_end());
}

void SplitCriticalEdgesPass::runOnOperation() {
  OpBuilder b(&getContext());
  SmallVector<Block *> blocks;
  for (Block &block : getOperation().getBody())
    blocks.push_back(&block);
  for (Block *block : blocks) {
    auto br = dyn_cast<CondBrOp>(block->getTerminator());
    if (!br)
      continue;
    for (unsigned i = 0; i < 2; ++i) {
      Block *target = br->getSuccessor(i);
      if (numPredecessorEdges(target) < 2)
        continue;
      SuccessorOperands args = br.getSuccessorOperands(i);
      SmallVector<Value> forwarded(args.getForwardedOperands());
      Block *edge = b.createBlock(target);
      b.setInsertionPointToEnd(edge);
      BrOp::create(b, br.getLoc(), forwarded, target);
      args.erase(0, args.size());
      br->setSuccessor(edge, i);
    }
  }
  for (Block &block : getOperation().getBody())
    if (numPredecessorEdges(&block) > 1)
      for (Block *pred : block.getPredecessors())
        if (pred->getNumSuccessors() > 1) {
          (void)postconditionFailed("sea-split-critical-edges",
                                    "a critical edge", pred->getTerminator());
          return signalPassFailure();
        }
}

} // namespace

std::unique_ptr<Pass> createSplitCriticalEdgesPass() {
  return std::make_unique<SplitCriticalEdgesPass>();
}

} // namespace sea
