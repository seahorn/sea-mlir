// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SinkAssumes.cpp - every sea.assume -> one assume before the assert -===//
//
// An alternative form of the assumes of a BMC-form program, for the solver
// (a selectable mode, sea-vc --sink-assumes; default off). The VC encodes
// an assume `c` in block B as the side condition `reach(B) -> c`; this pass
// replaces all of them with one assume before the final assert, of the
// conjunction of the assumes on the path taken, carried along the CFG as
// an i1 value (SeaHorn's UnifyAssumes, --horn-unify-assumes, does the same
// on LLVM IR; this is its sea-mlir counterpart, for programs exported with
// it off):
//     acc(entry) = true
//     acc_in(B)  = acc_out(P) if B has one predecessor P (or all of B's
//                  predecessors carry the same value); else a new i1 block
//                  argument, each predecessor passing its acc_out
//     acc_out(B) = acc_in(B) & c_1 & ... & c_k   (B's assumes, in order)
//     before the final sea.assert:  sea.assume acc_out(exit)
// The original assumes are erased.
//
// Why it is exact (both directions). In BMC form the program has one
// assert, right before the one halt, so an execution either fails that
// assert or not, at the end; an execution violating an assume is
// excluded, whenever that happens. Let a complete execution run the path
// B_0 ... B_n (B_n the exit block). It satisfies every original assume on
// its way iff it satisfies acc_out(B_n), the conjunction of exactly the
// assumes of B_0 ... B_n: the value passed along an edge is the one of the
// block it leaves. Each condition is an SSA value, so its value at the end
// is its value at the assume (a load is a read of an SSA memory value, not
// of a later memory). Blocks that never reach the exit (they end in
// sea.unreachable) lose their assumes: such a block cannot execute in a
// complete execution anyway. Print ops change nothing (the VC encodes no
// term for them), and a replay runs on a model that satisfies the final
// assume, hence every original one.
//
// Contract.
//   Accepts: a program in BMC form (loop-free; one sea.halt with the one
//     sea.assert right before it, print ops aside), or a program with
//     neither (statically safe: left alone); anything else is the error
//     sea-sink-assumes.form.
//   Produces: at most one sea.assume, right before the assert (none if the
//     program had none); the CFG's edges are unchanged (so no critical edge
//     is added), blocks may gain one i1 argument. Checked.
//   Reports: the number of assumes moved (createSinkAssumesPass(&n)).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/RegionGraphTraits.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetVector.h"

using namespace mlir;

namespace sea {
namespace {

class SinkAssumesPass
    : public PassWrapper<SinkAssumesPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SinkAssumesPass)

  explicit SinkAssumesPass(unsigned *moved = nullptr) : moved(moved) {}

  StringRef getArgument() const final { return "sea-sink-assumes"; }
  StringRef getDescription() const final {
    return "Replace every sea.assume of a BMC-form program by one assume of "
           "the conjunction of the assumes on the path, before the final "
           "assert (SeaHorn's UnifyAssumes)";
  }

  void runOnOperation() override;

private:
  unsigned *moved;
};

LogicalResult formError(ProgramOp program, const Twine &what) {
  diag::error("sea-sink-assumes.form",
              "sea-sink-assumes needs BMC form: " + what, program.getLoc());
  return failure();
}

void SinkAssumesPass::runOnOperation() {
  ProgramOp program = getOperation();
  Region &body = program.getBody();
  SmallVector<AssumeOp> all;
  SmallVector<AssertOp> asserts;
  SmallVector<HaltOp> halts;
  program.walk([&](AssumeOp a) { all.push_back(a); });
  program.walk([&](AssertOp a) { asserts.push_back(a); });
  program.walk([&](HaltOp h) { halts.push_back(h); });
  if (moved)
    *moved = 0;
  // no assert and no halt (the statically safe BMC form: every path is
  // blocked): no path to sink the assumes along, nothing to do
  if (all.empty() || (asserts.empty() && halts.empty()))
    return;
  if (asserts.size() != 1 || halts.size() != 1 ||
      nextNonPrintOp(asserts.front()) != halts.front().getOperation()) {
    (void)formError(program, "one sea.assert right before the one sea.halt");
    return signalPassFailure();
  }
  AssertOp finalAssert = asserts.front();
  Block *exitBlock = finalAssert->getBlock();

  MLIRContext *ctx = &getContext();
  auto i1 = IntegerType::get(ctx, 1);
  OpBuilder b(ctx);
  // acc_out per block; null = true (no assume on any path so far)
  llvm::DenseMap<Block *, Value> accOut;
  llvm::ReversePostOrderTraversal<Block *> rpot(&body.front());
  llvm::DenseSet<Block *> reached(rpot.begin(), rpot.end());
  for (Block *blk : rpot) {
    Value acc;
    if (blk != &body.front()) {
      // predecessors unreachable from the entry never run: they pass true
      llvm::SetVector<Block *> preds;
      for (Block *p : blk->getPredecessors())
        if (reached.count(p))
          preds.insert(p);
      bool same = true;
      for (Block *p : preds) {
        auto it = accOut.find(p);
        if (it == accOut.end()) {
          // a predecessor after blk in reverse post-order: a back edge
          (void)formError(program, "the CFG has a loop");
          return signalPassFailure();
        }
        if (it->second != accOut.lookup(preds.front()))
          same = false;
      }
      for (Block *p : blk->getPredecessors())
        if (!reached.count(p))
          same = false; // (rare) give them an operand too
      if (preds.empty()) {
        (void)formError(program, "a reached block without a reached "
                                 "predecessor");
        return signalPassFailure();
      }
      if (same) {
        // the one value of every predecessor: its definition dominates
        // every predecessor, hence blk
        acc = accOut.lookup(preds.front());
      } else {
        acc = blk->addArgument(i1, program.getLoc());
        llvm::SetVector<Block *> allPreds(blk->pred_begin(), blk->pred_end());
        for (Block *p : allPreds) {
          Operation *term = p->getTerminator();
          auto br = cast<BranchOpInterface>(term);
          Value v = accOut.lookup(p);
          if (!v) {
            b.setInsertionPoint(term);
            v = ConstOp::create(b, term->getLoc(), i1,
                                IntegerAttr::get(i1, APInt(1, 1)));
          }
          for (unsigned s = 0, e = term->getNumSuccessors(); s < e; ++s)
            if (term->getSuccessor(s) == blk)
              br.getSuccessorOperands(s).append(v);
        }
      }
    }
    for (Operation &op : llvm::make_early_inc_range(*blk)) {
      auto a = dyn_cast<AssumeOp>(op);
      if (!a)
        continue;
      Value c = a.getCond();
      if (!acc) {
        acc = c;
      } else {
        b.setInsertionPoint(a);
        acc = AndOp::create(b, a.getLoc(), acc, c);
      }
      a.erase();
      if (moved)
        ++*moved;
    }
    accOut[blk] = acc;
  }
  // assumes in blocks the traversal never reached (unreachable from the
  // entry): they cannot execute
  program.walk([&](AssumeOp a) {
    a.erase();
    if (moved)
      ++*moved;
  });
  if (Value acc = accOut.lookup(exitBlock)) {
    b.setInsertionPoint(finalAssert);
    AssumeOp::create(b, finalAssert.getLoc(), acc);
  }
  // postcondition: at most one assume, right before the assert
  unsigned n = 0;
  bool placed = true;
  program.walk([&](AssumeOp a) {
    ++n;
    placed = placed && a->getNextNode() == finalAssert.getOperation();
  });
  if (n > 1 || !placed) {
    (void)postconditionFailed("sea-sink-assumes",
                              "an assume other than the final one", program);
    return signalPassFailure();
  }
}

} // namespace

std::unique_ptr<Pass> createSinkAssumesPass(unsigned *moved) {
  return std::make_unique<SinkAssumesPass>(moved);
}

} // namespace sea
