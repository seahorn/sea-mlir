// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- MergeAsserts.cpp - many sea.assert ops -> the one BMC-form assert -===//
//
// Multi-assert support (the merge strategy of ctac's ua pass).
// A program may hold any number of sea.assert ops
// (an instrumentation job inserts one per checked access); the VC wants
// BMC form: one sea.assert right before the one sea.halt. This pass makes
// one exit block
//     ^exit(%ok: i1):  sea.assert %ok ; sea.halt
// and redirects:
//   - every sea.halt  ->  sea.br ^exit(true)
//   - every `sea.assert P` (its block B split after it):
//         B:      ... ; sea.cond_br P, ^cont, ^land
//         ^cont:  sea.assume P ; (the rest of B)
//         ^land:  sea.br ^exit(false)        (the assert's location)
//     The landing block keeps the edge into ^exit non-critical.
// An execution fails the merged assert iff it fails one original assert
// (the first one on its path); the landing block's location names it.
// The cond_br and the landing br carry the assert's location wrapped as
// loc("sea.assert"(<loc>)) (kMergedAssertLocName): a replay reports it as
// the failing assert, not as a branch of the source.
//
// Contract.
//   Accepts: a program with any asserts and halts (loops are fine: the
//     pass is structural).
//   Produces: one sea.assert, immediately before the only sea.halt, in the
//     exit block; no other assert, no other halt (checked). A program that
//     is already in that shape is left alone.
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

class MergeAssertsPass
    : public PassWrapper<MergeAssertsPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(MergeAssertsPass)

  StringRef getArgument() const final { return "sea-merge-asserts"; }
  StringRef getDescription() const final {
    return "Redirect every sea.assert to one exit block with the only "
           "assert and halt (BMC form; ctac ua merge)";
  }

  void runOnOperation() override;
};

void MergeAssertsPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<AssertOp> asserts;
  SmallVector<HaltOp> halts;
  program.walk([&](AssertOp a) { asserts.push_back(a); });
  program.walk([&](HaltOp h) { halts.push_back(h); });
  // already BMC form: one assert right before the one halt
  if (asserts.size() == 1 && halts.size() == 1 &&
      nextNonPrintOp(asserts.front()) == halts.front().getOperation())
    return;
  MLIRContext *ctx = &getContext();
  OpBuilder b(ctx);
  auto i1 = IntegerType::get(ctx, 1);
  auto boolConst = [&](Location loc, bool v) -> Value {
    return ConstOp::create(b, loc, i1, IntegerAttr::get(i1, APInt(1, v)));
  };
  // nothing to check (an instrumentation job with no check): `assert true`
  // before the halt(s), so the program is in BMC form and trivially holds
  if (asserts.empty()) {
    for (HaltOp h : halts) {
      b.setInsertionPoint(h);
      asserts.push_back(
          AssertOp::create(b, h.getLoc(), boolConst(h.getLoc(), true)));
    }
    if (asserts.size() == 1)
      return;
  }
  Region &body = program.getBody();
  Location loc = program.getLoc();
  Block *exit = new Block();
  body.push_back(exit);
  Value ok = exit->addArgument(i1, loc);
  b.setInsertionPointToEnd(exit);
  AssertOp::create(b, loc, ok);
  HaltOp::create(b, loc);
  for (HaltOp h : halts) {
    b.setInsertionPoint(h);
    Value t = boolConst(h.getLoc(), true);
    BrOp::create(b, h.getLoc(), ValueRange{t}, exit);
    h.erase();
  }
  for (AssertOp a : asserts) {
    Block *blk = a->getBlock();
    Location aloc = a.getLoc();
    Location brLoc =
        NameLoc::get(StringAttr::get(ctx, kMergedAssertLocName), aloc);
    Value cond = a.getCond();
    // everything after the assert moves to ^cont
    Block *cont = blk->splitBlock(a->getNextNode());
    Block *land = new Block();
    body.getBlocks().insertAfter(Region::iterator(blk), land);
    b.setInsertionPointToEnd(land);
    Value f = boolConst(aloc, false);
    BrOp::create(b, brLoc, ValueRange{f}, exit);
    b.setInsertionPointToStart(cont);
    AssumeOp::create(b, aloc, cond);
    b.setInsertionPoint(a);
    CondBrOp::create(b, brLoc, cond, ValueRange{}, ValueRange{}, cont, land);
    a.erase();
  }
  // postcondition: one assert, right before the only halt
  unsigned na = 0, nh = 0;
  program.walk([&](AssertOp) { ++na; });
  program.walk([&](HaltOp) { ++nh; });
  if (na != 1 || nh != 1) {
    (void)postconditionFailed("sea-merge-asserts", "several asserts or halts",
                              program);
    return signalPassFailure();
  }
}

} // namespace

std::unique_ptr<Pass> createMergeAssertsPass() {
  return std::make_unique<MergeAssertsPass>();
}

} // namespace sea
