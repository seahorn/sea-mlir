// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- DropFreshMemHavoc.cpp - memhavoc of fresh objects is a no-op -*- C++
//-*-===//
//
// The contents of a fresh object are unconstrained already, so a memhavoc
// of it before anything wrote to it changes nothing: it is removed. That is
// SeaHorn's sound use of memhavoc (protecting a fresh allocation); a
// memhavoc elsewhere stays and is modeled as such (the VC: constant
// lengths only).
//
// Fresh: the memhavoc's pointer comes only from allocations (sea.alloca /
// sea.malloc, through ptr.add, select and block arguments) or null (a
// havoc of null touches nothing), and for each such allocation A no update
// on the memory chain back to A's allocation may write A: walking
// back from the memhavoc's memory operand, every update that A dominates
// writes an object provably not A (its pointer comes from another
// allocation); updates not dominated by A precede the object. Block
// arguments: all incoming versions must be fresh; a cycle is not.
//
// Contract.
//   Accepts: abstract programs (pointer-indexed memory); runs before
//     sea-lower-tracking: a memhavoc of a fresh object is a no-op, so not a
//     modification either (verify-c-common allocates and havocs inputs
//     after sea_tracking_on and expects them unmodified, as SeaHorn, whose
//     memhavoc writes nothing).
//   Produces: no memhavoc on a fresh object (by the rule above); the others
//     unchanged.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Dominance.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class DropFreshMemHavocPass
    : public PassWrapper<DropFreshMemHavocPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DropFreshMemHavocPass)

  StringRef getArgument() const final { return "sea-drop-fresh-memhavoc"; }
  StringRef getDescription() const final {
    return "Remove memhavoc of objects nothing has written since their "
           "allocation (a no-op)";
  }

  void runOnOperation() override;
};

/// The allocations a pointer may point into: through ptr.add, select and
/// block arguments (every incoming value); null adds none (a memhavoc of
/// null touches nothing). False if some source is not an allocation.
static bool allocationsOf(Value p, llvm::SmallPtrSetImpl<Operation *> &allocs,
                          llvm::DenseSet<Value> &seen, unsigned depth = 0) {
  if (depth > 64)
    return false;
  if (!seen.insert(p).second)
    return true; // joins and cycles: the other paths decide
  if (auto arg = dyn_cast<BlockArgument>(p)) {
    Block *b = arg.getOwner();
    if (b->hasNoPredecessors())
      return false;
    for (auto it = b->pred_begin(); it != b->pred_end(); ++it) {
      auto br = dyn_cast<BranchOpInterface>((*it)->getTerminator());
      Value in = br ? br.getSuccessorOperands(
                          it.getSuccessorIndex())[arg.getArgNumber()]
                    : Value();
      if (!in || !allocationsOf(in, allocs, seen, depth + 1))
        return false;
    }
    return true;
  }
  Operation *def = p.getDefiningOp();
  if (isa<AllocaOp, MallocOp>(def)) {
    allocs.insert(def);
    return true;
  }
  if (isa<PtrNullOp>(def))
    return true;
  if (auto add = dyn_cast<PtrAddOp>(def))
    return allocationsOf(add.getPtr(), allocs, seen, depth + 1);
  if (auto pi = dyn_cast<PtrIndexOp>(def))
    return allocationsOf(pi.getBase(), allocs, seen, depth + 1);
  if (auto sel = dyn_cast<SelectOp>(def))
    return allocationsOf(sel.getTrueValue(), allocs, seen, depth + 1) &&
           allocationsOf(sel.getFalseValue(), allocs, seen, depth + 1);
  return false;
}

/// The allocation a pointer points into, if it is exactly one.
static Operation *allocationOf(Value p) {
  llvm::SmallPtrSet<Operation *, 4> allocs;
  llvm::DenseSet<Value> seen;
  if (!allocationsOf(p, allocs, seen) || allocs.size() != 1)
    return nullptr;
  return *allocs.begin();
}

/// No update on the chain ending in `mem` that `alloc` dominates may write
/// `alloc`'s object. `memo`: decided values; a value being decided (a
/// cycle) counts as not fresh.
static bool fresh(Value mem, Operation *alloc, DominanceInfo &dom,
                  llvm::DenseMap<Value, std::optional<bool>> &memo) {
  auto [it, inserted] = memo.try_emplace(mem, std::nullopt);
  if (!inserted)
    return it->second.value_or(false);
  bool r = false;
  if (auto arg = dyn_cast<BlockArgument>(mem)) {
    Block *b = arg.getOwner();
    r = true;
    for (auto p = b->pred_begin(); r && p != b->pred_end(); ++p) {
      auto br = dyn_cast<BranchOpInterface>((*p)->getTerminator());
      Value in = br ? br.getSuccessorOperands(
                          p.getSuccessorIndex())[arg.getArgNumber()]
                    : Value();
      r = in && fresh(in, alloc, dom, memo);
    }
  } else {
    Operation *def = mem.getDefiningOp();
    if (isa<MemNondetOp, MemConstOp>(def) || !dom.properlyDominates(alloc, def))
      r = true; // the start, or before the object exists
    else if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(
                 def)) {
      Operation *target = allocationOf(def->getOperand(1));
      r = target && target != alloc &&
          fresh(def->getOperand(0), alloc, dom, memo);
    }
  }
  memo[mem] = r;
  return r;
}

void DropFreshMemHavocPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<MemHavocOp> havocs;
  program.walk([&](MemHavocOp op) {
    if (isa<PtrType>(op.getAddr().getType()))
      havocs.push_back(op);
  });
  if (havocs.empty())
    return;
  DominanceInfo dom(program);
  for (MemHavocOp op : havocs) {
    llvm::SmallPtrSet<Operation *, 4> allocs;
    llvm::DenseSet<Value> seen;
    if (!allocationsOf(op.getAddr(), allocs, seen))
      continue;
    bool allFresh = true;
    for (Operation *alloc : allocs) {
      llvm::DenseMap<Value, std::optional<bool>> memo;
      allFresh = allFresh && fresh(op.getMem(), alloc, dom, memo);
    }
    if (!allFresh)
      continue;
    op.getResult().replaceAllUsesWith(op.getMem());
    op.erase();
  }
}

} // namespace

std::unique_ptr<Pass> createDropFreshMemHavocPass() {
  return std::make_unique<DropFreshMemHavocPass>();
}

} // namespace sea
