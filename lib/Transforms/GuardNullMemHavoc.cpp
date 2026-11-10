// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- GuardNullMemHavoc.cpp - memhavoc of null is a no-op ------*- C++ -*-===//
//
// An open semantics decision (docs/design/Memory-Step2.md): sea.memhavoc
// whose pointer is null itself does nothing, as the interpreter runs it
// (RunOptions::memhavocNullNoop). Placement makes null address 0, where the
// VC would havoc the cells from address 0 on, which may be the first
// object's (the compact layout starts objects at 1). This pass makes the
// rule explicit in the program, so every VC encoding (store chains,
// lambdas, SeaM's range terms) follows it: a memhavoc whose pointer may be
// null runs only when the pointer is not null,
//     B:      ... ; %z = ptr.cmp eq %p, null ; cond_br %z, ^j(%m), ^h
//     ^h:     %m1 = memhavoc %m[%p], %n ; br ^j(%m1)
//     ^j(%mj): (the rest of B)
// null + k (k != 0) is not null: its memhavoc stays unguarded, as the
// interpreter does not treat it as a no-op either.
//
// Contract.
//   Accepts: abstract programs (pointer-indexed memory), after
//     sea-drop-fresh-memhavoc (a dropped memhavoc needs no guard) and
//     before sea-lower-tracking (a guarded-off memhavoc marks nothing).
//   Produces: every memhavoc whose pointer may be null (it is not, through
//     ptr.add / ptr.index bases, select and block arguments, only
//     allocations, globals and function addresses) in a block of its own,
//     entered only when the pointer is not null. The number of guards is
//     set on the program (kMemhavocNullGuardsAttr); sea-vc reports it.
//     Leaves critical edges (sea-split-critical-edges runs later).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class GuardNullMemHavocPass
    : public PassWrapper<GuardNullMemHavocPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(GuardNullMemHavocPass)

  StringRef getArgument() const final { return "sea-guard-null-memhavoc"; }
  StringRef getDescription() const final {
    return "Run a memhavoc only when its pointer is not null (memhavoc of "
           "null is a no-op, an open semantics decision)";
  }

  void runOnOperation() override;
};

/// May `p` be the null pointer? No only if every source (through ptr.add /
/// ptr.index bases, select and block arguments) is an object: an
/// allocation, a global, a function address.
static bool mayBeNull(Value p) {
  llvm::DenseSet<Value> seen;
  SmallVector<Value> work{p};
  while (!work.empty()) {
    Value v = work.pop_back_val();
    if (!seen.insert(v).second)
      continue;
    if (auto arg = dyn_cast<BlockArgument>(v)) {
      Block *b = arg.getOwner();
      if (b->hasNoPredecessors())
        return true;
      for (auto it = b->pred_begin(); it != b->pred_end(); ++it) {
        auto br = dyn_cast<BranchOpInterface>((*it)->getTerminator());
        Value in = br ? br.getSuccessorOperands(
                            it.getSuccessorIndex())[arg.getArgNumber()]
                      : Value();
        if (!in)
          return true;
        work.push_back(in);
      }
      continue;
    }
    Operation *def = v.getDefiningOp();
    if (isa<AllocaOp, MallocOp, GlobalOp, FuncAddrOp>(def))
      continue;
    if (auto add = dyn_cast<PtrAddOp>(def)) {
      work.push_back(add.getPtr());
      continue;
    }
    if (auto pi = dyn_cast<PtrIndexOp>(def)) {
      work.push_back(pi.getBase());
      continue;
    }
    if (auto sel = dyn_cast<SelectOp>(def)) {
      work.push_back(sel.getTrueValue());
      work.push_back(sel.getFalseValue());
      continue;
    }
    return true; // null, a loaded or nondet pointer, anything else
  }
  return false;
}

void GuardNullMemHavocPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<MemHavocOp> havocs;
  program.walk([&](MemHavocOp op) {
    if (isa<PtrType>(op.getAddr().getType()) && mayBeNull(op.getAddr()))
      havocs.push_back(op);
  });
  Region &body = program.getBody();
  OpBuilder b(&getContext());
  for (MemHavocOp op : havocs) {
    Location loc = op.getLoc();
    Block *blk = op->getBlock();
    Value mem = op.getMem();
    // the rest of the block, after the memhavoc, starts the join
    Block *join = blk->splitBlock(op->getNextNode());
    Value mj = join->addArgument(mem.getType(), loc);
    op.getResult().replaceAllUsesWith(mj);
    Block *havoc = new Block();
    body.getBlocks().insertAfter(Region::iterator(blk), havoc);
    op->moveBefore(havoc, havoc->end());
    b.setInsertionPointToEnd(havoc);
    BrOp::create(b, loc, ValueRange{op.getResult()}, join);
    b.setInsertionPointToEnd(blk);
    Value null = PtrNullOp::create(b, loc, op.getAddr().getType());
    Value isNull =
        PtrCmpOp::create(b, loc, ICmpPredicate::eq, op.getAddr(), null);
    CondBrOp::create(b, loc, isNull, ValueRange{mem}, ValueRange{}, join,
                     havoc);
  }
  // postcondition: each guarded memhavoc is entered only from the false
  // side of a cond_br on `ptr.cmp eq <its pointer>, null`
  for (MemHavocOp op : havocs) {
    Block *pred = op->getBlock()->getSinglePredecessor();
    auto cbr = pred ? dyn_cast<CondBrOp>(pred->getTerminator()) : CondBrOp();
    auto cmp = cbr ? cbr.getCondition().getDefiningOp<PtrCmpOp>() : PtrCmpOp();
    if (!cmp || cbr.getFalseDest() != op->getBlock() ||
        cmp.getLhs() != op.getAddr() || !cmp.getRhs().getDefiningOp<PtrNullOp>()) {
      (void)postconditionFailed("sea-guard-null-memhavoc",
                                "a memhavoc that may be of null is unguarded",
                                op);
      return signalPassFailure();
    }
  }
  if (!havocs.empty())
    program->setAttr(kMemhavocNullGuardsAttr,
                     IntegerAttr::get(IntegerType::get(&getContext(), 64),
                                      havocs.size()));
}

} // namespace

std::unique_ptr<Pass> createGuardNullMemHavocPass() {
  return std::make_unique<GuardNullMemHavocPass>();
}

} // namespace sea
