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
// havoc of null touches nothing), and for each such allocation A the
// memory chain back from the memhavoc's memory operand starts at a
// sea.mem.nondet (a sea.mem.const fixes the object's contents: its
// memhavoc is not a no-op) and no update on it may write A: an update
// that happens before A on every path (it properly dominates A, or A is
// reachable from it and not the other way) cannot; every other one must
// write an object provably not A (its pointer comes from one other
// allocation). An update not dominated by A may still follow it (a join
// after an allocation in one branch). Block arguments: all incoming
// versions must be fresh; a cycle is not.
//
// Contract.
//   Accepts: abstract programs (pointer-indexed memory); runs before
//     sea-lower-tracking: a memhavoc of a fresh object is a no-op, so not a
//     modification either (verify-c-common allocates and havocs inputs
//     after sea_tracking_on and expects them unmodified, as SeaHorn, whose
//     memhavoc writes nothing).
//   Produces: no memhavoc on a fresh object (by the rule above); the others
//     unchanged. Premise A-INB: a drop is sound only if no access of
//     another object lands in this one (after placement, objects share one
//     memory) and the memhavoc stays in its object. The drops that rest
//     on it (the chain passed an update, or the memhavoc's range is not
//     provably in its object) are counted on the program
//     (kDropFreshInbAttr); sea-vc reports them (assume.a-inb), also with
//     --seam-inb=0. A separate memory-safety check of the program would
//     discharge A-INB.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
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

/// Does `op` happen before `alloc` on every path that executes both: it
/// properly dominates `alloc`, or `alloc`'s block is reachable from its
/// block and not the other way (no cycle through both). An op merely not
/// dominated by `alloc` may follow it: a join block after an allocation in
/// one branch.
class HappensBefore {
public:
  explicit HappensBefore(DominanceInfo &dom) : dom(dom) {}
  bool operator()(Operation *op, Operation *alloc) {
    if (dom.properlyDominates(op, alloc))
      return true;
    Block *a = op->getBlock(), *b = alloc->getBlock();
    return a != b && reachable(a).contains(b) && !reachable(b).contains(a);
  }

private:
  const llvm::DenseSet<Block *> &reachable(Block *from) {
    auto [it, inserted] = cache.try_emplace(from);
    if (inserted) {
      SmallVector<Block *> work(from->getSuccessors().begin(),
                                from->getSuccessors().end());
      while (!work.empty()) {
        Block *b = work.pop_back_val();
        if (it->second.insert(b).second)
          work.append(b->getSuccessors().begin(), b->getSuccessors().end());
      }
    }
    return it->second;
  }
  DominanceInfo &dom;
  llvm::DenseMap<Block *, llvm::DenseSet<Block *>> cache;
};

/// No update on the chain ending in `mem` may write `alloc`'s object, and
/// the chain starts at a sea.mem.nondet: only there are a fresh object's
/// contents unconstrained (a sea.mem.const gives them its value, which a
/// memhavoc changes). An update that happens before `alloc`
/// (HappensBefore) cannot write the object: walked through unchecked.
/// Every other update must write an object provably not `alloc`'s (its
/// pointer comes from one other allocation). `memo`: decided values; a
/// value being decided (a cycle) counts as not fresh.
static bool fresh(Value mem, Operation *alloc, HappensBefore &before,
                  llvm::DenseMap<Value, std::optional<bool>> &memo) {
  auto [it, inserted] = memo.try_emplace(mem, std::nullopt);
  if (!inserted)
    return it->second.value_or(false);
  // the linear part of the chain, iteratively (chains can be long)
  SmallVector<Value> walked{mem};
  Value cur = mem;
  bool r = false;
  while (true) {
    if (auto arg = dyn_cast<BlockArgument>(cur)) {
      Block *b = arg.getOwner();
      r = !b->hasNoPredecessors();
      for (auto p = b->pred_begin(); r && p != b->pred_end(); ++p) {
        auto br = dyn_cast<BranchOpInterface>((*p)->getTerminator());
        Value in = br ? br.getSuccessorOperands(
                            p.getSuccessorIndex())[arg.getArgNumber()]
                      : Value();
        r = in && fresh(in, alloc, before, memo);
      }
      break;
    }
    Operation *def = cur.getDefiningOp();
    if (isa<MemNondetOp>(def)) {
      r = true;
      break;
    }
    if (!isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(def)) {
      r = false; // a sea.mem.const, or an unknown producer
      break;
    }
    if (!before(def, alloc)) {
      Operation *target = allocationOf(def->getOperand(1));
      if (!target || target == alloc) {
        r = false;
        break;
      }
    }
    cur = def->getOperand(0);
    auto [cit, cnew] = memo.try_emplace(cur, std::nullopt);
    if (!cnew) {
      r = cit->second.value_or(false);
      break;
    }
    walked.push_back(cur);
  }
  for (Value v : walked)
    memo[v] = r;
  return r;
}

/// Does the chain ending in `mem` pass an update before it reaches its
/// roots (through block arguments)?
static bool hasUpdate(Value mem) {
  llvm::DenseSet<Value> seen;
  SmallVector<Value> work{mem};
  while (!work.empty()) {
    Value v = work.pop_back_val();
    if (!seen.insert(v).second)
      continue;
    if (auto arg = dyn_cast<BlockArgument>(v)) {
      Block *b = arg.getOwner();
      for (auto p = b->pred_begin(); p != b->pred_end(); ++p) {
        auto br = dyn_cast<BranchOpInterface>((*p)->getTerminator());
        if (!br)
          return true; // decided not fresh before; be conservative
        work.push_back(
            br.getSuccessorOperands(p.getSuccessorIndex())[arg.getArgNumber()]);
      }
      continue;
    }
    if (!isa<MemNondetOp>(v.getDefiningOp()))
      return true;
  }
  return false;
}

/// Does the memhavoc's range provably lie in its object: its address is an
/// allocation's result (offset 0) and its constant length at most the
/// allocation's constant size?
static bool rangeInObject(MemHavocOp op) {
  Operation *def = op.getAddr().getDefiningOp();
  if (!isa_and_present<AllocaOp, MallocOp>(def))
    return false;
  llvm::APInt len, size;
  if (!matchPattern(op.getLen(), m_ConstantInt(&len)) ||
      !matchPattern(def->getOperand(0), m_ConstantInt(&size)))
    return false;
  return len.getActiveBits() <= 64 && size.getActiveBits() <= 64 &&
         len.getZExtValue() <= size.getZExtValue();
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
  HappensBefore before(dom);
  uint64_t inbDrops = 0;
  for (MemHavocOp op : havocs) {
    llvm::SmallPtrSet<Operation *, 4> allocs;
    llvm::DenseSet<Value> seen;
    if (!allocationsOf(op.getAddr(), allocs, seen))
      continue;
    bool allFresh = true;
    for (Operation *alloc : allocs) {
      llvm::DenseMap<Value, std::optional<bool>> memo;
      allFresh = allFresh && fresh(op.getMem(), alloc, before, memo);
    }
    if (!allFresh)
      continue;
    // A-INB: the drop is sound only if no access lands in the object from
    // outside: an update the walk passed (another object's, or one before
    // the allocation) must stay in its object, and so must the memhavoc
    // itself (else it changed a neighbor). After placement objects share
    // one memory. With no update and the range in the object, the
    // object's cells are the initial memory's: unconstrained already.
    if (hasUpdate(op.getMem()) || !rangeInObject(op))
      ++inbDrops;
    op.getResult().replaceAllUsesWith(op.getMem());
    op.erase();
  }
  if (inbDrops)
    program->setAttr(
        kDropFreshInbAttr,
        IntegerAttr::get(IntegerType::get(&getContext(), 64), inbDrops));
}

} // namespace

std::unique_ptr<Pass> createDropFreshMemHavocPass() {
  return std::make_unique<DropFreshMemHavocPass>();
}

} // namespace sea
