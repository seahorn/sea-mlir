// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ReachTarget.cpp - R_T, the reach program of one target ---*- C++ -*-===//
//
// R_T for a reach target T (sea/Interp/ReachTargets.h): the program with
// every assert dropped and an assert(false) at the top of T. An input
// fails R_T's assertion iff its execution reaches the top of T with every
// earlier assume holding, which is what sea-vc --reach asks of T; so sea-vc
// on R_T answers sat iff T is reachable, and the witness of a reachable T
// fails R_T's assertion in sea-run. R_T is an ordinary program: the safety
// query, sea-run, the fuzzer and query-diff take it as they are.
//
// T's block is split right before T and the part before it ends in a
// sea.br to a new block `sea.assert false; sea.halt` (T's location, the
// assert tagged sea.reach_target = T's id); T and what followed it in its
// block are gone, and so is every block no longer reachable from the entry.
// Every other sea.assert is erased and every other sea.halt becomes
// sea.unreachable: a run that ends without reaching T is blocked. A target
// no path from the entry reaches gives a program without an assertion and
// without a sea.halt (the statically safe form: unsat).
//
// Contract.
//   Accepts: any program; the target by id (`id`, `kind`=checks|blocks);
//     an id the program does not have is an error (reach-target.id).
//   Produces: one sea.halt, right after the only sea.assert, whose
//     condition is the constant false, or (a target no path reaches) no
//     sea.halt and no sea.assert (checked); the new block has one
//     predecessor, which ends in a sea.br: no critical edge, so BMC form
//     is kept (a loop-free input stays loop-free).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Interp/ReachTargets.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/RegionUtils.h"

using namespace mlir;

namespace sea {

/// Is `target` in a block a path from the entry reaches?
static bool onSomePath(Region &body, Block *target) {
  llvm::SmallPtrSet<Block *, 16> seen;
  SmallVector<Block *> work{&body.front()};
  while (!work.empty()) {
    Block *b = work.pop_back_val();
    if (b == target)
      return true;
    if (seen.insert(b).second)
      llvm::append_range(work, b->getSuccessors());
  }
  return false;
}

LogicalResult buildReachTargetProgram(ProgramOp program, Operation *target,
                                      StringRef id) {
  Region &body = program.getBody();
  Location loc = target->getLoc();
  OpBuilder b(program.getContext());
  Block *head = target->getBlock();
  // no path reaches T (its block has no predecessors on one): R_T has no
  // assertion left, the statically safe form (sea-vc: unsat)
  const bool dead = !onSomePath(body, head);
  AssertOp check;
  HaltOp halt;
  Value no;
  if (!dead) {
    Block *rest = head->splitBlock(target);
    Block *err = new Block();
    body.push_back(err);
    b.setInsertionPointToEnd(err);
    Type i1 = b.getI1Type();
    no = ConstOp::create(b, loc, i1, IntegerAttr::get(i1, APInt(1, 0)));
    check = AssertOp::create(b, loc, no);
    check->setAttr(kReachTargetAttr, b.getStringAttr(id));
    halt = HaltOp::create(b, loc);
    b.setInsertionPointToEnd(head);
    auto br = BrOp::create(b, loc, ValueRange{}, err);
    // block ids (kBlockIdsAttr), if the program has them: the head keeps
    // its block's (the rest goes); the new block is T's (id t)
    if (ArrayAttr ids = blockIdsOf(rest)) {
      br->setAttr(kBlockIdsAttr, ids);
      BlockIdNamer::set(halt, BlockIdNamer(program).fresh("t"));
    }
  }
  // T and the rest of its block, and whatever only they reached
  IRRewriter rewriter(program.getContext());
  (void)eraseUnreachableBlocks(rewriter, body);
  SmallVector<Operation *> asserts, halts;
  program.walk([&](Operation *op) {
    if (isa<AssertOp>(op) && op != check.getOperation())
      asserts.push_back(op);
    else if (isa<HaltOp>(op) && op != halt.getOperation())
      halts.push_back(op);
  });
  for (Operation *a : asserts)
    a->erase();
  for (Operation *h : halts) {
    b.setInsertionPoint(h);
    copyBlockIds(h->getBlock(), UnreachableOp::create(b, h->getLoc()));
    h->erase();
  }
  // the postcondition
  unsigned nHalts = 0, nAsserts = 0;
  program.walk([&](Operation *op) {
    nHalts += isa<HaltOp>(op);
    nAsserts += isa<AssertOp>(op);
  });
  if (dead) {
    if (nHalts || nAsserts)
      return postconditionFailed("sea-reach-target",
                                 "a sea.assert or sea.halt left for a target "
                                 "no path reaches",
                                 program);
    return success();
  }
  if (nHalts != 1 || nAsserts != 1 ||
      nextNonPrintOp(check) != halt.getOperation() ||
      check.getCond().getDefiningOp() != no.getDefiningOp())
    return postconditionFailed("sea-reach-target",
                               "not one sea.assert false right before the "
                               "only sea.halt",
                               program);
  return success();
}

namespace {

class ReachTargetPass
    : public PassWrapper<ReachTargetPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ReachTargetPass)

  StringRef getArgument() const final { return "sea-reach-target"; }
  StringRef getDescription() const final {
    return "R_T: drop every assert and put an assert(false) at the top of "
           "the reach target `id` (sea-vc --reach ids; kind=checks|blocks)";
  }

  ReachTargetPass() = default;
  ReachTargetPass(const ReachTargetPass &o) : PassWrapper(o) {}
  ReachTargetPass(std::string target, bool blocks) {
    id = target;
    kind = blocks ? "blocks" : "checks";
  }

  void runOnOperation() override {
    ProgramOp program = getOperation();
    if (kind != "checks" && kind != "blocks") {
      diag::error("reach-target.kind",
                  "sea-reach-target: kind is checks or blocks, not '" + kind +
                      "'",
                  program.getLoc());
      return signalPassFailure();
    }
    std::vector<ReachTarget> targets = collectReachTargets(
        program, kind == "blocks" ? ReachKind::Blocks : ReachKind::Checks);
    auto it = llvm::find_if(targets,
                            [&](const ReachTarget &t) { return t.id == id; });
    if (it == targets.end()) {
      diag::error("reach-target.id",
                  "sea-reach-target: the program has no " + kind +
                      " target with id '" + id +
                      "' (sea-vc --reach lists them)",
                  program.getLoc());
      return signalPassFailure();
    }
    if (failed(buildReachTargetProgram(program, it->op, it->id)))
      return signalPassFailure();
  }

  Option<std::string> id{*this, "id",
                         llvm::cl::desc("the reach target's id (sea-vc "
                                        "--reach's reach: lines)")};
  Option<std::string> kind{*this, "kind",
                           llvm::cl::desc("checks (default) or blocks"),
                           llvm::cl::init("checks")};
};

} // namespace

std::unique_ptr<Pass> createReachTargetPass(std::string id, bool blocks) {
  return std::make_unique<ReachTargetPass>(std::move(id), blocks);
}

std::unique_ptr<Pass> createReachTargetPass() {
  return std::make_unique<ReachTargetPass>();
}

} // namespace sea
