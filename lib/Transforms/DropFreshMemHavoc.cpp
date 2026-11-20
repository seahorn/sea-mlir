// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- DropFreshMemHavoc.cpp - memhavoc of fresh objects is a no-op -*- C++
//-*-===//
//
// The contents of a fresh object are unconstrained already, so a memhavoc
// of it before anything wrote or read them changes nothing: it is removed.
// That is SeaHorn's sound use of memhavoc (protecting a fresh allocation);
// a memhavoc elsewhere stays and is modeled as such (the VC: constant
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
// Unread: no read may have seen A's cells. A read (sea.load, the source of
// sea.memmove) of a version that descends from the chain's roots without
// passing the memhavoc (the chain, or a version that branches off it) whose
// pointer may come from A, or from somewhere unknown, makes the memhavoc
// observable (a later read of the object may differ from it), unless it
// happens before A. Such a memhavoc stays. The walk skips users in blocks
// and branch edges that no run through the memhavoc passes (a join after
// a path around the allocation), but does not order reads against the
// memhavoc: a read of an earlier version after it also keeps it (coarse,
// never unsound).
//
// Contract.
//   Accepts: abstract programs (pointer-indexed memory); runs before
//     sea-lower-tracking: a memhavoc of a fresh object is a no-op, so not a
//     modification either (verify-c-common allocates and havocs inputs
//     after sea_tracking_on and expects them unmodified, as SeaHorn, whose
//     memhavoc writes nothing).
//   Produces: no memhavoc on a fresh, unread object (by the rules above);
//     the others unchanged. Premise A-SEP (no cell accessed through two
//     provenances): a drop is sound only if no access through another
//     object touches a cell this one's accesses touch (after placement,
//     objects share one memory), the memhavoc's own range included. The
//     drops that rest on it are counted on the program (kDropFreshInbAttr):
//     every drop with an access (a read or a write, through any pointer) of
//     the versions the unread walk visits, or whose range is not provably in
//     its object (rangeInObject: the pointer is an allocation's result or
//     null, through select and block arguments, and the length at most the
//     allocation's size: constants, or one value up to `mul x, 1`). sea-vc
//     reports them (assume.a-sep), also with
//     --seam-premises=0. A separate memory-safety (bounds) check of the
//     program discharges A-INB, which implies A-SEP.
//   Null: a pointer that may be null itself (through select and block
//     arguments; null + k is not null, and a memhavoc of it is kept) follows
//     --memhavoc-null. Under noop (the default) a memhavoc of null touches
//     nothing: such a drop rests on that open decision and is counted with
//     sea-guard-null-memhavoc's guards (kMemhavocNullGuardsAttr, reported
//     as assume.memhavoc-null-noop: vc sites). Under error the memhavoc is
//     kept, so the run on null stays an error.
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

  explicit DropFreshMemHavocPass(bool memhavocNullNoop)
      : memhavocNullNoop(memhavocNullNoop) {}

  StringRef getArgument() const final { return "sea-drop-fresh-memhavoc"; }
  StringRef getDescription() const final {
    return "Remove memhavoc of objects nothing has written or read since "
           "their allocation (a no-op)";
  }

  void runOnOperation() override;

private:
  /// --memhavoc-null=noop: a memhavoc of null is a no-op
  bool memhavocNullNoop;
};

/// Values visited by allocationsOf, with whether under an offset.
using PtrSeen = llvm::DenseSet<std::pair<Value, unsigned>>;

/// The allocations a pointer may point into: through ptr.add, ptr.index,
/// select and block arguments (every incoming value). Null itself adds none
/// and sets `null` (a memhavoc of null touches nothing under
/// --memhavoc-null=noop); null + k (under ptr.add / ptr.index) is no object:
/// false, as when some source is not an allocation.
static bool allocationsOf(Value p, llvm::SmallPtrSetImpl<Operation *> &allocs,
                          PtrSeen &seen, bool *null = nullptr,
                          bool offset = false, unsigned depth = 0) {
  if (depth > 64)
    return false;
  if (!seen.insert({p, offset ? 1u : 0u}).second)
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
      if (!in || !allocationsOf(in, allocs, seen, null, offset, depth + 1))
        return false;
    }
    return true;
  }
  Operation *def = p.getDefiningOp();
  if (isa<AllocaOp, MallocOp>(def)) {
    allocs.insert(def);
    return true;
  }
  if (isa<PtrNullOp>(def)) {
    if (offset)
      return false;
    if (null)
      *null = true;
    return true;
  }
  if (auto add = dyn_cast<PtrAddOp>(def))
    return allocationsOf(add.getPtr(), allocs, seen, null, true, depth + 1);
  if (auto pi = dyn_cast<PtrIndexOp>(def))
    return allocationsOf(pi.getBase(), allocs, seen, null, true, depth + 1);
  if (auto sel = dyn_cast<SelectOp>(def))
    return allocationsOf(sel.getTrueValue(), allocs, seen, null, offset,
                         depth + 1) &&
           allocationsOf(sel.getFalseValue(), allocs, seen, null, offset,
                         depth + 1);
  return false;
}

/// The allocation a pointer points into, if it is exactly one.
static Operation *allocationOf(Value p) {
  llvm::SmallPtrSet<Operation *, 4> allocs;
  PtrSeen seen;
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
  /// Is there a path of one or more edges from `a` to `b`?
  bool reaches(Block *a, Block *b) { return reachable(a).contains(b); }

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

/// The roots of the chain ending in `mem`: walked back through updates and
/// the incoming values of block arguments, a root is any other value (after
/// fresh(), a sea.mem.nondet; a memhavoc of null only skips that check).
static void rootsOf(Value mem, SmallVectorImpl<Value> &roots) {
  llvm::DenseSet<Value> seen;
  SmallVector<Value> work{mem};
  while (!work.empty()) {
    Value v = work.pop_back_val();
    if (!seen.insert(v).second)
      continue;
    if (auto arg = dyn_cast<BlockArgument>(v)) {
      Block *b = arg.getOwner();
      bool known = !b->hasNoPredecessors();
      SmallVector<Value> ins;
      for (auto p = b->pred_begin(); known && p != b->pred_end(); ++p) {
        auto br = dyn_cast<BranchOpInterface>((*p)->getTerminator());
        known = br != nullptr;
        if (known)
          ins.push_back(br.getSuccessorOperands(
              p.getSuccessorIndex())[arg.getArgNumber()]);
      }
      if (known)
        work.append(ins.begin(), ins.end());
      else
        roots.push_back(v);
      continue;
    }
    Operation *def = v.getDefiningOp();
    if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(def))
      work.push_back(def->getOperand(0));
    else
      roots.push_back(v);
  }
}

/// The accesses of the memory that may see its cells as they are before
/// the memhavoc, in a run that executes it: the users of every version that
/// descends from the chain's roots without passing the memhavoc's result
/// (the chain itself, and versions that branch off it: a load may read an
/// earlier version, a non-inplace update makes a copy), as far as a path
/// through the memhavoc can hold them: a user in a block no such path
/// passes, or a branch edge none takes (a join after a path that skips the
/// allocation and the memhavoc), is not followed. Coarse otherwise: the
/// order of an access relative to the memhavoc is not looked at, and a
/// join reached both through the memhavoc and around it counts.
struct PriorAccesses {
  /// some load, store, memset, memmove or other memhavoc
  bool any = false;
  /// a read that may read a cell of one of the memhavoc's objects: its
  /// pointer may come from that allocation, or from somewhere unknown (and
  /// it does not happen before the allocation); also any user the walk does
  /// not know
  bool readsObject = false;
};

static PriorAccesses
priorAccesses(MemHavocOp op, const llvm::SmallPtrSetImpl<Operation *> &allocs,
              HappensBefore &before) {
  PriorAccesses acc;
  Block *hb = op->getBlock();
  Region *top = hb->getParent();
  // can a run execute both the memhavoc and an op in `b`?
  auto withHavoc = [&](Block *b) {
    return b == hb || before.reaches(b, hb) || before.reaches(hb, b);
  };
  // can a run take the edge from `p` to `s` and execute the memhavoc?
  auto edgeWithHavoc = [&](Block *p, Block *s) {
    return p == hb || before.reaches(hb, p) || s == hb || before.reaches(s, hb);
  };
  auto read = [&](Operation *user, Value ptr) {
    acc.any = true;
    llvm::SmallPtrSet<Operation *, 4> from;
    PtrSeen seen;
    bool known = allocationsOf(ptr, from, seen);
    for (Operation *alloc : allocs)
      if ((!known || from.contains(alloc)) && !before(user, alloc))
        acc.readsObject = true;
  };
  SmallVector<Value> work;
  rootsOf(op.getMem(), work);
  llvm::DenseSet<Value> seen;
  while (!work.empty() && !acc.readsObject) {
    Value v = work.pop_back_val();
    if (!seen.insert(v).second)
      continue;
    for (OpOperand &use : v.getUses()) {
      Operation *user = use.getOwner();
      if (user == op.getOperation())
        continue; // its result and what descends only from it come after
      // the op (or the region op holding it) in the memhavoc's region
      Operation *at = top->findAncestorOpInRegion(*user);
      if (at && !withHavoc(at->getBlock()))
        continue; // no run executes both
      if (auto load = dyn_cast<LoadOp>(user)) {
        read(user, load.getAddr());
      } else if (auto mv = dyn_cast<MemMoveOp>(user)) {
        if (&use == &mv.getSrcMutable()) {
          read(user, mv.getSrcAddr());
        } else {
          acc.any = true;
          work.push_back(mv.getResult());
        }
      } else if (isa<StoreOp, MemSetOp, MemHavocOp>(user)) {
        acc.any = true;
        work.push_back(user->getResult(0));
      } else if (isa<ResetModifiedOp>(user)) {
        work.push_back(user->getResult(0)); // no cell changes
      } else if (isa<IsModifiedOp, PrintMemOp>(user)) {
        // the set of modified objects, not the cells; a print changes nothing
      } else if (auto br = dyn_cast<BranchOpInterface>(user)) {
        if (auto arg = br.getSuccessorBlockArgument(use.getOperandNumber())) {
          if (at != user || edgeWithHavoc(user->getBlock(), arg->getOwner()))
            work.push_back(*arg);
        } else
          acc.readsObject = true;
      } else {
        acc.any = acc.readsObject = true; // an unknown user: assume it reads
      }
    }
  }
  return acc;
}

/// The allocations a pointer is the result of, at offset 0: through select
/// and block arguments (every incoming value); null adds none (a memhavoc
/// of null touches nothing). False if some source is anything else.
static bool objectStarts(Value p, llvm::SmallPtrSetImpl<Operation *> &allocs,
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
      if (!in || !objectStarts(in, allocs, seen, depth + 1))
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
  if (auto sel = dyn_cast<SelectOp>(def))
    return objectStarts(sel.getTrueValue(), allocs, seen, depth + 1) &&
           objectStarts(sel.getFalseValue(), allocs, seen, depth + 1);
  return false;
}

/// `v` without the factors `mul x, 1` around it (the front end's
/// malloc(n * sizeof(char))): the same bit-vector value.
static Value withoutMulOne(Value v) {
  for (unsigned i = 0; i < 8; ++i) {
    auto mul = v.getDefiningOp<MulOp>();
    if (!mul)
      break;
    if (matchPattern(mul.getRhs(), m_One()))
      v = mul.getLhs();
    else if (matchPattern(mul.getLhs(), m_One()))
      v = mul.getRhs();
    else
      break;
  }
  return v;
}

/// Is `len` at most `size`: one value (up to `mul x, 1`), or two constants?
static bool lenFits(Value len, Value size) {
  if (len.getType() == size.getType() &&
      withoutMulOne(len) == withoutMulOne(size))
    return true;
  llvm::APInt l, n;
  if (!matchPattern(len, m_ConstantInt(&l)) ||
      !matchPattern(size, m_ConstantInt(&n)))
    return false;
  return l.getActiveBits() <= 64 && n.getActiveBits() <= 64 &&
         l.getZExtValue() <= n.getZExtValue();
}

/// Does the memhavoc's range provably lie in its object? Its pointer is an
/// allocation's result (offset 0; through select and block arguments, or
/// null) and its length at most that allocation's size (lenFits). Cheap and
/// syntactic: a range it cannot decide is reported as not in its object.
static bool rangeInObject(MemHavocOp op) {
  llvm::SmallPtrSet<Operation *, 4> allocs;
  llvm::DenseSet<Value> seen;
  if (!objectStarts(op.getAddr(), allocs, seen))
    return false;
  for (Operation *alloc : allocs)
    if (!lenFits(op.getLen(), alloc->getOperand(0)))
      return false;
  return true;
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
  uint64_t inbDrops = 0, nullDrops = 0;
  for (MemHavocOp op : havocs) {
    llvm::SmallPtrSet<Operation *, 4> allocs;
    PtrSeen seen;
    bool mayBeNull = false;
    if (!allocationsOf(op.getAddr(), allocs, seen, &mayBeNull))
      continue;
    // --memhavoc-null=error: a memhavoc of null is an error, not a no-op;
    // it stays so the run on null keeps it
    if (mayBeNull && !memhavocNullNoop)
      continue;
    bool allFresh = true;
    for (Operation *alloc : allocs) {
      llvm::DenseMap<Value, std::optional<bool>> memo;
      allFresh = allFresh && fresh(op.getMem(), alloc, before, memo);
    }
    if (!allFresh)
      continue;
    // A read of the object since its allocation makes the memhavoc
    // observable: a later read may see other values. Then it stays.
    PriorAccesses acc = priorAccesses(op, allocs, before);
    if (acc.readsObject)
      continue;
    // A-SEP: the drop is sound only if no access through another provenance
    // touches the object's cells (after placement objects share one
    // memory): an access before the memhavoc (a write the chain walk passed,
    // a read of another object, or one before the allocation) must not, and
    // the memhavoc's cells outside its object (if any) are touched by no
    // other provenance. With no such access and the range in the object,
    // the object's cells are the initial memory's: unconstrained already,
    // and nothing saw them.
    if (acc.any || !rangeInObject(op))
      ++inbDrops;
    // a pointer that may be null: the drop rests on memhavoc(null) being a
    // no-op, counted with sea-guard-null-memhavoc's guards
    if (mayBeNull)
      ++nullDrops;
    op.getResult().replaceAllUsesWith(op.getMem());
    op.erase();
  }
  auto i64 = IntegerType::get(&getContext(), 64);
  if (inbDrops)
    program->setAttr(kDropFreshInbAttr, IntegerAttr::get(i64, inbDrops));
  if (nullDrops) {
    if (auto n = program->getAttrOfType<IntegerAttr>(kMemhavocNullGuardsAttr))
      nullDrops += n.getInt();
    program->setAttr(kMemhavocNullGuardsAttr, IntegerAttr::get(i64, nullDrops));
  }
}

} // namespace

std::unique_ptr<Pass> createDropFreshMemHavocPass(bool memhavocNullNoop) {
  return std::make_unique<DropFreshMemHavocPass>(memhavocNullNoop);
}

} // namespace sea
