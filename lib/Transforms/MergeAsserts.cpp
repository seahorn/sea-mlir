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
// the failing assert, not as a branch of the source. Both carry the
// assert's id attribute (kAssertIdAttr), if it has one; the assume in
// ^cont carries its reach site (kReachSiteAttr), if it has one; under
// kReachFailuresAttr the landing block starts with the reach-site marker
// of its failure (kAssertFailMarker; inert, a print).
//
// Contract.
//   Accepts: a program with any asserts and halts (loops are fine: the
//     pass is structural).
//   Reports: every assert of constant 0 (warning fold.assert-false) but
//     the error-location marker of SeaHorn's error slice (the only assert,
//     right before a sea.halt, print ops aside, with no source position:
//     unknown or a .mlir position); not with report=false,
//     the lowering's second run, so each is reported once; an assert whose
//     condition becomes 0 only in the lowering's canonicalize is not
//     reported: its cond_br folds (the verdict still shows it).
//   Option: report (default true; sea-opt
//     --sea-merge-asserts='report=false'): the warnings above.
//   Produces: one sea.assert, immediately before the only sea.halt, in the
//     exit block; no other assert, no other halt (checked). A program that
//     is already in that shape is left alone.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class MergeAssertsPass
    : public PassWrapper<MergeAssertsPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(MergeAssertsPass)

  MergeAssertsPass() = default;
  MergeAssertsPass(const MergeAssertsPass &other) : PassWrapper(other) {}
  explicit MergeAssertsPass(bool report) { reportConstFalse = report; }

  StringRef getArgument() const final { return "sea-merge-asserts"; }
  StringRef getDescription() const final {
    return "Redirect every sea.assert to one exit block with the only "
           "assert and halt (BMC form; ctac ua merge)";
  }

  void runOnOperation() override;

  Option<bool> reportConstFalse{
      *this, "report",
      llvm::cl::desc("Warn fold.assert-false for each assert of constant 0 "
                     "(off in the lowering's second run: reported once)"),
      llvm::cl::init(true)};
};

void MergeAssertsPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<AssertOp> asserts;
  SmallVector<HaltOp> halts;
  program.walk([&](AssertOp a) { asserts.push_back(a); });
  program.walk([&](HaltOp h) { halts.push_back(h); });
  // an assert of constant 0 fails wherever it is reached; once merged it
  // is a constant cond_br that canonicalize folds silently, so it is
  // reported here (as the sea.assert canonicalizer reports one it folds).
  // Not the error-location marker of SeaHorn's error slice: the program's
  // only assert, of constant 0, right before a sea.halt, with no source
  // position (reaching it is the failure; the guards on the path are the
  // checks). A located one is a real check (sccp folded it to 0).
  auto hasSourcePosition = [](Location loc) {
    bool found = false;
    loc->walk([&](Location l) {
      if (auto f = dyn_cast<FileLineColLoc>(l);
          f && !f.getFilename().getValue().ends_with(".mlir"))
        found = true;
      return found ? WalkResult::interrupt() : WalkResult::advance();
    });
    return found;
  };
  auto isErrorMarker = [&](AssertOp a) {
    Operation *next = nextNonPrintOp(a);
    return asserts.size() == 1 && next && isa<HaltOp>(next) &&
           !hasSourcePosition(a.getLoc());
  };
  for (AssertOp a : asserts)
    if (reportConstFalse && matchPattern(a.getCond(), m_Zero()) &&
        !isErrorMarker(a))
      diag::warn("fold.assert-false",
                 "assert of constant false: the check fails whenever it is "
                 "reached",
                 a.getLoc());
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
    diag::warn("merge-asserts.no-assertions",
               "no assertions remain; the assertion check is trivially safe",
               program.getLoc());
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
    // a tagged assert's failure: a reach target (sea-vc --reach-failures)
    if (Attribute site = a->getAttr(kReachSiteAttr);
        site && program->hasAttr(kReachFailuresAttr))
      createReachSiteMarker(b, aloc, kAssertFailMarker, site);
    Value f = boolConst(aloc, false);
    BrOp::create(b, brLoc, ValueRange{f}, exit);
    b.setInsertionPointToStart(cont);
    auto pass = AssumeOp::create(b, aloc, cond);
    // the assert's reach site (kReachSiteAttr), on the assume: reached
    // once the assert holds
    if (Attribute site = a->getAttr(kReachSiteAttr))
      pass->setAttr(kReachSiteAttr, site);
    b.setInsertionPoint(a);
    auto cbr = CondBrOp::create(b, brLoc, cond, ValueRange{}, ValueRange{},
                                cont, land);
    // the assert's id (kAssertIdAttr), on both branches made of it
    if (Attribute id = a->getAttr(kAssertIdAttr)) {
      cbr->setAttr(kAssertIdAttr, id);
      land->getTerminator()->setAttr(kAssertIdAttr, id);
    }
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

std::unique_ptr<Pass> createMergeAssertsPass(bool reportConstFalse) {
  return std::make_unique<MergeAssertsPass>(reportConstFalse);
}

} // namespace sea
