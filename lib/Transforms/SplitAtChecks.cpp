// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SplitAtChecks.cpp - a block right before each check site -*- C++ -*-===//
//
// The block guard of the VC says that a block executes, and every assume of
// the block is a fact under it. sea-vc --reach asks whether an execution
// reaches the top of a check site (or of a block) with every earlier assume
// holding: the guard of a block that ends right before the site, holding no
// check site itself, is that question exactly. This pass makes that block:
// each check site starts a block of its own, entered by a sea.br from the
// block that held the ops before it (the head: the original block, with its
// arguments and predecessors, or the block of the previous check site). With
// `blocks`, every block is split at its front instead: the head is empty.
// sea-vc --reach runs it after the lowering (lib/VC/Reach.cpp).
//
// Contract.
//   Accepts: any program.
//   Produces: every sea.assume, sea.assert and sea.unreachable is the first
//     op of a block whose only predecessor ends in a sea.br to it and holds
//     no other check site than its first op (checked); `blocks`: every original
//     block is a head holding only a sea.br. Each split adds the sea.br (the
//     location of the first op moved) to a new block with one predecessor: no
//     critical edge, BMC form kept; values are unchanged.
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

static bool isCheckSite(Operation *op) {
  return isa<AssumeOp, AssertOp, UnreachableOp>(op) || isReachSiteMarker(op);
}

class SplitAtChecksPass
    : public PassWrapper<SplitAtChecksPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitAtChecksPass)

  SplitAtChecksPass(bool blocks = false, unsigned *splits = nullptr)
      : blocks(blocks), splits(splits) {}

  StringRef getArgument() const final { return "sea-split-at-checks"; }
  StringRef getDescription() const final {
    return "Start a new block at every sea.assume, sea.assert and "
           "sea.unreachable, or (blocks) at the front of every block "
           "(sea-vc --reach)";
  }

  void runOnOperation() override {
    SmallVector<Operation *> sites;
    for (Block &b : getOperation().getBody())
      for (Operation &op : b)
        if (blocks ? &op == &b.front() : isCheckSite(&op))
          sites.push_back(&op);
    OpBuilder b(&getContext());
    for (Operation *op : sites) {
      Block *head = op->getBlock();
      Block *rest = head->splitBlock(op);
      b.setInsertionPointToEnd(head);
      BrOp::create(b, op->getLoc(), ValueRange{}, rest);
    }
    if (splits)
      *splits += sites.size();
    if (blocks) {
      // every original block is a head holding only a sea.br to the rest,
      // whose only predecessor it is
      for (Operation *op : sites) {
        Block *rest = op->getBlock();
        Block *head = rest->getSinglePredecessor();
        if (!head || &rest->front() != op || !llvm::hasSingleElement(*head) ||
            !isa<BrOp>(head->front())) {
          (void)postconditionFailed("sea-split-at-checks",
                                    "a block not split at its front", op);
          return signalPassFailure();
        }
      }
      return;
    }
    for (Block &block : getOperation().getBody())
      for (Operation &op : block) {
        if (!isCheckSite(&op))
          continue;
        Block *pred = block.getSinglePredecessor();
        // the head holds the ops between the previous check site (its first
        // op, if any) and this one
        bool ok = &block.front() == &op && pred &&
                  isa<BrOp>(pred->getTerminator()) &&
                  llvm::none_of(llvm::drop_begin(*pred),
                                [](Operation &o) { return isCheckSite(&o); });
        if (!ok) {
          (void)postconditionFailed("sea-split-at-checks",
                                    "a check site without a block of its own "
                                    "right before it",
                                    &op);
          return signalPassFailure();
        }
      }
  }

private:
  bool blocks;
  unsigned *splits;
};

} // namespace

namespace {
class SplitAssertFailuresPass
    : public PassWrapper<SplitAssertFailuresPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitAssertFailuresPass)

  StringRef getArgument() const final { return "sea-split-assert-failures"; }
  StringRef getDescription() const final {
    return "Give the failure of each sea.assert with a reach site a block "
           "of its own, holding the reach-site marker of the failure "
           "(sea-vc --reach)";
  }

  void runOnOperation() override {
    SmallVector<AssertOp> asserts;
    getOperation().walk([&](AssertOp a) {
      if (a->hasAttr(kReachSiteAttr))
        asserts.push_back(a);
    });
    OpBuilder b(&getContext());
    Region &body = getOperation().getBody();
    for (AssertOp a : asserts) {
      Location loc = a.getLoc();
      Block *head = a->getBlock();
      Block *chk = head->splitBlock(a);
      Block *pass = new Block(), *fail = new Block();
      body.getBlocks().insert(Region::iterator(chk), pass);
      body.getBlocks().insert(Region::iterator(chk), fail);
      b.setInsertionPointToEnd(pass);
      BrOp::create(b, loc, ValueRange{}, chk);
      b.setInsertionPointToEnd(fail);
      createReachSiteMarker(b, loc, kAssertFailMarker,
                            a->getAttr(kReachSiteAttr));
      BrOp::create(b, loc, ValueRange{}, chk);
      b.setInsertionPointToEnd(head);
      CondBrOp::create(b, loc, a.getCond(), ValueRange{}, ValueRange{}, pass,
                       fail);
    }
  }
};
} // namespace

std::unique_ptr<Pass> createSplitAssertFailuresPass() {
  return std::make_unique<SplitAssertFailuresPass>();
}

std::unique_ptr<Pass> createSplitAtChecksPass(bool blocks, unsigned *splits) {
  return std::make_unique<SplitAtChecksPass>(blocks, splits);
}

} // namespace sea
