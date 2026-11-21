// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SplitAsserts.cpp - many sea.assert ops -> one program per assert --===//
//
// The counterpart of sea-merge-asserts. Where merging folds every assert of
// a program into one BMC-form assert (one query for all of them), splitting
// makes one program per assert (one query each). For a program @p with k
// asserts a_0 .. a_{k-1} (program order: block order, then op order), part
// i is a copy @p.assert<i> in which
//   - a_i stays (with its attributes);
//   - every other assert becomes `sea.assume` of its condition (with the
//     assert's attributes): on a path to a_i the earlier asserts are
//     assumed (assume-guarantee: part j checks a_j, so a_i is checked on
//     the executions that pass every assert before it);
//   - the block of a_i ends right after it, with `sea.halt` (what comes
//     after a_i does not decide whether it fails);
//   - only a_i's block and its CFG ancestors are kept (the cone of a_i); a
//     `cond_br c` of a kept block with one target outside the cone becomes
//     `sea.assume c` (or its negation) and a branch to the other target.
// Part i fails iff an execution of @p reaches a_i with its condition false
// and every assert it passed on the way holding. So @p fails some assert
// iff some part fails (the first assert an execution fails is a_i of a
// part that fails), and @p is safe iff every part is.
//
// Each part carries kSplitAssertAttr = {index = i, count = k}. An assert
// that is unreachable in the CFG (no path from the entry) gives a part that
// is `assert true` (the assert's location and attributes): it cannot fail
// (warning split-asserts.unreachable). A program with no assert is left
// alone (no part).
//
// Contract.
//   Accepts: a module of sea.program ops; option `program`: split only
//     the program of that name (the others stay). The block of a split
//     assert must not lie on a CFG cycle (error split-asserts.cycle).
//   Produces: in place of each split program with k > 0 asserts, its k
//     parts, right after where it was; each part has exactly one assert,
//     followed by sea.halt in its block, and no other halt (checked). The
//     parts are not in BMC form yet: sea-merge-asserts (a no-op on them
//     but for the halts it already finds alone) and the usual lowering
//     follow.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/SetVector.h"

using namespace mlir;

namespace sea {
namespace {

class SplitAssertsPass
    : public PassWrapper<SplitAssertsPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitAssertsPass)

  SplitAssertsPass() = default;
  SplitAssertsPass(const SplitAssertsPass &o) : PassWrapper(o) {}
  explicit SplitAssertsPass(std::string only) { program = only; }

  StringRef getArgument() const final { return "sea-split-asserts"; }
  StringRef getDescription() const final {
    return "One program per sea.assert: that assert kept, the others "
           "assumed, pruned to the assert's cone (the counterpart of "
           "sea-merge-asserts)";
  }

  void runOnOperation() override;

  Option<std::string> program{
      *this, "program",
      llvm::cl::desc("Split only the program of this name (default: all)"),
      llvm::cl::init("")};
};

/// Part `index` of `p` (a fresh copy, in place); failure after an error.
LogicalResult makePart(ProgramOp part, unsigned index) {
  MLIRContext *ctx = part.getContext();
  OpBuilder b(ctx);
  auto i1 = IntegerType::get(ctx, 1);
  SmallVector<AssertOp> asserts;
  part.walk([&](AssertOp a) { asserts.push_back(a); });
  AssertOp live = asserts[index];
  // every other assert: an assume of its condition
  for (AssertOp a : asserts) {
    if (a == live)
      continue;
    b.setInsertionPoint(a);
    auto as = AssumeOp::create(b, a.getLoc(), a.getCond());
    as->setDiscardableAttrs(a->getDiscardableAttrDictionary());
    a.erase();
  }
  Region &body = part.getBody();
  Block *home = live->getBlock();
  // the cone: home and its ancestors
  llvm::SetVector<Block *> cone;
  SmallVector<Block *> work{home};
  bool cycle = false;
  while (!work.empty()) {
    Block *blk = work.pop_back_val();
    for (Block *pred : blk->getPredecessors()) {
      if (pred == home)
        cycle = true;
      if (pred != home && cone.insert(pred))
        work.push_back(pred);
    }
  }
  if (cycle) {
    diag::error("split-asserts.cycle",
                "sea-split-asserts: the assert's block lies on a CFG cycle",
                live.getLoc());
    return failure();
  }
  cone.insert(home);
  if (!cone.contains(&body.front())) {
    // no path from the entry: the assert cannot fail
    diag::warn("split-asserts.unreachable",
               "an assert no path from the entry reaches: its part cannot "
               "fail",
               live.getLoc());
    Location loc = live.getLoc();
    DictionaryAttr attrs = live->getDiscardableAttrDictionary();
    for (Block &blk : body)
      blk.dropAllDefinedValueUses();
    for (Block &blk : body)
      for (Operation &op : blk)
        op.dropAllReferences();
    while (!body.empty())
      body.front().erase();
    Block *entry = new Block();
    body.push_back(entry);
    b.setInsertionPointToEnd(entry);
    Value t = ConstOp::create(b, loc, i1, IntegerAttr::get(i1, 1));
    auto a = AssertOp::create(b, loc, t);
    a->setDiscardableAttrs(attrs);
    HaltOp::create(b, loc);
    return success();
  }
  // the tail of home after the assert, then sea.halt
  SmallVector<Operation *> tail;
  for (Operation *op = live->getNextNode(); op; op = op->getNextNode())
    tail.push_back(op);
  SmallVector<Block *> dead;
  for (Block &blk : body)
    if (!cone.contains(&blk))
      dead.push_back(&blk);
  for (Operation *op : tail)
    op->dropAllUses();
  for (Block *blk : dead)
    blk->dropAllDefinedValueUses();
  for (Operation *op : llvm::reverse(tail))
    op->erase();
  for (Block *blk : dead)
    for (Operation &op : *blk)
      op.dropAllReferences();
  // branches of the cone that leave it
  for (Block *blk : cone) {
    if (blk == home)
      continue;
    auto cbr = dyn_cast<CondBrOp>(blk->getTerminator());
    if (!cbr)
      continue;
    bool inT = cone.contains(cbr.getTrueDest()),
         inF = cone.contains(cbr.getFalseDest());
    if (inT && inF)
      continue;
    // an ancestor of home has a successor in the cone
    Location loc = cbr.getLoc();
    b.setInsertionPoint(cbr);
    Value c = cbr.getCondition();
    if (!inT) {
      Value one = ConstOp::create(b, loc, i1, IntegerAttr::get(i1, 1));
      c = XorOp::create(b, loc, i1, c, one);
    }
    AssumeOp::create(b, loc, c);
    auto br =
        inT ? BrOp::create(b, loc, cbr.getTrueDestOperands(), cbr.getTrueDest())
            : BrOp::create(b, loc, cbr.getFalseDestOperands(),
                           cbr.getFalseDest());
    br->setDiscardableAttrs(cbr->getDiscardableAttrDictionary());
    cbr.erase();
  }
  for (Block *blk : dead)
    blk->erase();
  b.setInsertionPointAfter(live);
  HaltOp::create(b, live.getLoc());
  return success();
}

void SplitAssertsPass::runOnOperation() {
  ModuleOp module = getOperation();
  SmallVector<ProgramOp> todo;
  for (auto p : module.getOps<ProgramOp>())
    if (program.empty() || p.getSymName() == program)
      todo.push_back(p);
  OpBuilder b(&getContext());
  for (ProgramOp p : todo) {
    unsigned k = 0;
    p.walk([&](AssertOp) { ++k; });
    if (k == 0)
      continue;
    b.setInsertionPointAfter(p);
    for (unsigned i = 0; i < k; ++i) {
      auto part = cast<ProgramOp>(b.clone(*p.getOperation()));
      part.setSymName((p.getSymName() + ".assert" + Twine(i)).str());
      part->setAttr(kSplitAssertAttr,
                    b.getDictionaryAttr(
                        {b.getNamedAttr("index", b.getI64IntegerAttr(i)),
                         b.getNamedAttr("count", b.getI64IntegerAttr(k))}));
      if (failed(makePart(part, i)))
        return signalPassFailure();
      // postcondition: one assert, right before the only halt
      unsigned na = 0, nh = 0;
      part.walk([&](AssertOp) { ++na; });
      part.walk([&](HaltOp) { ++nh; });
      AssertOp a;
      part.walk([&](AssertOp x) { a = x; });
      if (na != 1 || nh != 1 || !isa_and_nonnull<HaltOp>(a->getNextNode())) {
        (void)postconditionFailed("sea-split-asserts",
                                  "a part without exactly one assert "
                                  "before its only halt",
                                  part);
        return signalPassFailure();
      }
      b.setInsertionPointAfter(part);
    }
    p.erase();
  }
}

} // namespace

std::unique_ptr<Pass> createSplitAssertsPass(std::string program) {
  return std::make_unique<SplitAssertsPass>(std::move(program));
}

} // namespace sea
