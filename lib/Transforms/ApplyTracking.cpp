// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ApplyTracking.cpp - tracking markers -> `tracked` updates -*- C++
//-*-===//
//
// Modification tracking is switched on and off only at dominating
// positions (sea.tracking_on / sea.tracking_off markers that every
// execution reaching a later memory update passes), so whether an update
// is tracked is static: it is tracked iff the closest marker that
// dominates it is tracking_on. This pass sets `tracked` on
// those updates (store, memset, memmove, memhavoc) and erases the markers;
// it makes the start of tracking unconditional, so the set of modified
// objects starts empty there (no tracked update came before it).
//
// Contract.
//   Accepts: a program in which every memory update reachable from a
//     marker is dominated by it (tracking.position error otherwise; paths
//     that end early without an update may bypass it).
//   Produces: no marker (checked); `tracked` exactly on the updates whose
//     closest dominating marker is tracking_on.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Dominance.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class ApplyTrackingPass
    : public PassWrapper<ApplyTrackingPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ApplyTrackingPass)

  StringRef getArgument() const final { return "sea-apply-tracking"; }
  StringRef getDescription() const final {
    return "Turn sea.tracking_on/off markers into `tracked` on the memory "
           "updates between them";
  }

  void runOnOperation() override;
};

void ApplyTrackingPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<Operation *> markers;
  program.walk([&](Operation *op) {
    if (isa<TrackingOnOp, TrackingOffOp>(op))
      markers.push_back(op);
  });
  if (markers.empty())
    return;

  DominanceInfo dom(program);
  bool ok = true;
  auto isUpdate = [](Operation *op) {
    return isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp>(op);
  };
  // Every update a marker can reach must come after it on every path (be
  // dominated by it): then whether an update is tracked does not depend on
  // the path. (Paths that end early, e.g. at a failed assumption, may
  // bypass a marker: they reach no update.)
  for (Operation *m : markers) {
    llvm::SmallPtrSet<Block *, 16> reach;
    SmallVector<Block *> work(m->getBlock()->getSuccessors().begin(),
                              m->getBlock()->getSuccessors().end());
    while (!work.empty()) {
      Block *b = work.pop_back_val();
      if (!reach.insert(b).second)
        continue;
      work.append(b->getSuccessors().begin(), b->getSuccessors().end());
    }
    Operation *bad = nullptr;
    for (Block *b : reach)
      for (Operation &op : *b)
        if (!bad && isUpdate(&op) && !dom.properlyDominates(m, &op))
          bad = &op;
    if (bad) {
      diag::error("tracking.position",
                  m->getName().getStringRef() +
                      " is not on every path to a memory update after it "
                      "(tracking is switched only at dominating positions)",
                  m->getLoc());
      bad->emitRemark("this update can be reached with and without it");
      ok = false;
    }
  }
  if (!ok)
    return signalPassFailure();

  MLIRContext *ctx = &getContext();
  program.walk([&](Operation *op) {
    if (!isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp>(op))
      return;
    // the markers dominating op lie on its dominator path: take the last
    Operation *closest = nullptr;
    for (Operation *m : markers)
      if (dom.properlyDominates(m, op) &&
          (!closest || dom.properlyDominates(closest, m)))
        closest = m;
    if (closest && isa<TrackingOnOp>(closest))
      op->setAttr("tracked", UnitAttr::get(ctx));
  });
  for (Operation *m : markers)
    m->erase();
  program.walk([&](Operation *op) {
    if (isa<TrackingOnOp, TrackingOffOp>(op)) {
      (void)postconditionFailed("sea-apply-tracking", "a tracking marker", op);
      signalPassFailure();
    }
  });
}

} // namespace

std::unique_ptr<Pass> createApplyTrackingPass() {
  return std::make_unique<ApplyTrackingPass>();
}

} // namespace sea
