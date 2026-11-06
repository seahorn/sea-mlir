// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerFatPointers.cpp - object metadata as explicit values -*- C++
//-*-===//
//
// sea.ptr.base and sea.ptr.dereferenceable ask for a pointer's object,
// which an address does not keep. This pass makes the object metadata
// explicit at the abstract level, as SeaHorn's fat pointers (address,
// offset, size), so placement can then run unchanged: every pointer value
// gets two companion integers of the pointer width, its offset in its
// object and the object's size, computed alongside it:
//   alloca/malloc n, global      -> (0, size);  func_addr, null, from_int
//                                   -> (0, 0)
//   ptr.add p, o                 -> (off(p) + o, size(p))
//   select / block arguments     -> the same choice of companions
//   load of a pointer at q in m  -> read from m's companion memories at q
// and every memory that stores pointers gets two companion memories (the
// offset and size of the pointer stored at each address): metadata that
// travels with the data, so a pointer store writes all three, memmove
// copies all three, and memset / memhavoc zero the companions over their
// range. A companion memory has the keyed layout (a pointer-width cell at
// every byte address, independent cells), so its accesses are never split
// or widened. A companion access carries the data access's `align` (and
// memmove's src_align): the promise is about the address, which the two
// share. Without it the companion memories were never widened, and 63 % of
// their accesses on vcc were split into bytes in the measured corpus. Then
//   sea.ptr.base p               -> ptr.add p, -off(p)
//   sea.ptr.dereferenceable p, n -> p != null and off <= size and
//                                   n <= size - off
// After placement the triple is SeaHorn's (address, offset, size).
//
// Contract.
//   Accepts: abstract programs with structs scalarized (sea-scalarize-
//     structs) and no pointer-valued op other than those above
//     (fat.unsupported); pointers must not flow through unreachable blocks.
//   Produces: no sea.ptr.base / sea.ptr.dereferenceable (checked); the
//     companions are ordinary values and memories (dead ones are harmless).
//   Not modeled: an integer store into a pointer-holding memory leaves the
//     companions at that address unchanged; the abstract semantics stops on
//     a pointer load from such cells, so a validated program never reads
//     them.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/RegionGraphTraits.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/PostOrderIterator.h"

using namespace mlir;

namespace sea {
/// The type of a companion memory of data memory \p t: a cell of the
/// pointer width at every byte address (the keyed layout):
/// a companion access is one cell at the pointer's address, never split or
/// widened, and is independent of the data memory's layout.
static MemType companionType(MemType t) {
  unsigned w = bitWidthOf(t.getIndex());
  return MemType::get(t.getContext(), t.getIndex(),
                      IntegerType::get(t.getContext(), w), MemLayout::keyed);
}

namespace {

class LowerFatPointersPass
    : public PassWrapper<LowerFatPointersPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerFatPointersPass)

  StringRef getArgument() const final { return "sea-lower-fat-pointers"; }
  StringRef getDescription() const final {
    return "Make pointer offsets and object sizes explicit (fat pointers), "
           "so sea.ptr.base / dereferenceable can be placed";
  }

  void runOnOperation() override;

private:
  Value find(Value v) {
    Value &p = parent[v];
    if (!p)
      p = v;
    return p == v ? v : (p = find(p));
  }
  void unite(Value a, Value b) { parent[find(a)] = find(b); }

  llvm::DenseMap<Value, Value> parent;
};

/// Companions: of a pointer, its offset and size; of a memory, its offset
/// and size memories.
struct Pair {
  Value off, size;
};

void LowerFatPointersPass::runOnOperation() {
  ProgramOp program = getOperation();
  bool any = false;
  program.walk([&](Operation *op) {
    any = any || isa<PtrBaseOp, PtrDereferenceableOp>(op);
  });
  if (!any)
    return;
  parent.clear();
  auto isPtr = [](Value v) { return isa<PtrType>(v.getType()); };
  auto isMem = [](Value v) { return isa<MemType>(v.getType()); };

  // Memories that hold pointers (def-use classes).
  program.walk([&](Operation *op) {
    if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(op))
      unite(op->getOperand(0), op->getResult(0));
    if (auto mv = dyn_cast<MemMoveOp>(op))
      unite(mv.getSrc(), mv.getMem());
    if (auto br = dyn_cast<BranchOpInterface>(op))
      for (unsigned i = 0, n = op->getNumSuccessors(); i < n; ++i) {
        SuccessorOperands ops = br.getSuccessorOperands(i);
        for (unsigned j = 0, m = ops.size(); j < m; ++j)
          if (Value in = ops[j]; in && isMem(in))
            unite(in, op->getSuccessor(i)->getArgument(j));
      }
  });
  llvm::DenseSet<Value> holdsPtrs;
  bool ok = true;
  program.walk([&](Operation *op) {
    if (auto st = dyn_cast<StoreOp>(op); st && isPtr(st.getValue()))
      holdsPtrs.insert(find(st.getMem()));
    if (auto ld = dyn_cast<LoadOp>(op); ld && isPtr(ld.getResult()))
      holdsPtrs.insert(find(ld.getMem()));
    for (Type t : op->getResultTypes())
      if (isa<StructType>(t)) {
        diag::error("fat.unsupported",
                    "sea-lower-fat-pointers needs structs scalarized first",
                    op->getLoc());
        ok = false;
      }
  });
  if (!ok)
    return signalPassFailure();
  auto needsMem = [&](Value v) {
    return isMem(v) && holdsPtrs.contains(find(v));
  };

  MLIRContext *ctx = &getContext();
  OpBuilder b(ctx);
  llvm::DenseMap<Value, Pair> fat; // pointer or memory -> companions
  // fresh nondet sites, after every site in the program
  uint64_t nextSite = 0;
  program.walk([&](Operation *op) {
    if (auto site = op->getAttrOfType<IntegerAttr>("site"))
      nextSite = std::max<uint64_t>(nextSite, site.getInt() + 1);
  });
  auto intOf = [&](Value p) {
    return IntegerType::get(ctx, cast<PtrType>(p.getType()).getWidth());
  };
  auto cst = [&](Location loc, Type t, const APInt &v) -> Value {
    return ConstOp::create(b, loc, t, IntegerAttr::get(t, v));
  };
  auto zero = [&](Location loc, Type t) {
    return cst(loc, t, APInt::getZero(t.getIntOrFloatBitWidth()));
  };

  // Companion block arguments first, appended in the order of their values.
  Region &body = program.getBody();
  llvm::DenseMap<Block *, SmallVector<unsigned>> extraArgsOf;
  for (Block &block : body) {
    unsigned n = block.getNumArguments();
    for (unsigned j = 0; j < n; ++j) {
      BlockArgument a = block.getArgument(j);
      Type t1, t2;
      if (isPtr(a))
        t1 = t2 = intOf(a);
      else if (needsMem(a))
        t1 = t2 = companionType(cast<MemType>(a.getType()));
      else
        continue;
      Value off = block.addArgument(t1, a.getLoc());
      Value size = block.addArgument(t2, a.getLoc());
      fat[a] = {off, size};
      extraArgsOf[&block].push_back(j);
    }
  }

  llvm::DenseSet<Block *> visited;
  SmallVector<Operation *> toErase;
  auto lookup = [&](Value v, Operation *at) -> std::optional<Pair> {
    auto it = fat.find(v);
    if (it != fat.end())
      return it->second;
    diag::error("fat.unsupported",
                "sea-lower-fat-pointers: no offset/size for a value of " +
                    at->getName().getStringRef(),
                at->getLoc());
    ok = false;
    return std::nullopt;
  };
  for (Block *block : llvm::ReversePostOrderTraversal<Region *>(&body)) {
    visited.insert(block);
    for (Operation &opRef : llvm::make_early_inc_range(*block)) {
      Operation *op = &opRef;
      Location loc = op->getLoc();
      b.setInsertionPointAfter(op);
      if (isa<AllocaOp, MallocOp>(op)) {
        Value p = op->getResult(0);
        b.setInsertionPointAfter(op);
        fat[p] = {zero(loc, intOf(p)), op->getOperand(0)};
      } else if (auto g = dyn_cast<GlobalOp>(op)) {
        Type t = intOf(g.getResult());
        fat[g.getResult()] = {
            zero(loc, t),
            cst(loc, t, APInt(t.getIntOrFloatBitWidth(), g.getSize()))};
      } else if (isa<FuncAddrOp, PtrNullOp, PtrFromIntOp, PtrNondetOp>(op)) {
        Type t = intOf(op->getResult(0));
        fat[op->getResult(0)] = {zero(loc, t), zero(loc, t)};
      } else if (auto pa = dyn_cast<PtrAddOp>(op)) {
        auto in = lookup(pa.getPtr(), op);
        if (!in)
          continue;
        Value off =
            AddOp::create(b, loc, in->off.getType(), in->off, pa.getOffset());
        fat[pa.getResult()] = {off, in->size};
      } else if (auto pi = dyn_cast<PtrIndexOp>(op)) {
        // the offset companion moves by the op's offset, as plain arithmetic
        auto in = lookup(pi.getBase(), op);
        if (!in)
          continue;
        Value delta = ptrIndexOffsetValue(b, pi);
        Value off = AddOp::create(b, loc, in->off.getType(), in->off, delta);
        fat[pi.getResult()] = {off, in->size};
      } else if (auto s = dyn_cast<SelectOp>(op); s && isPtr(s.getResult())) {
        auto t = lookup(s.getTrueValue(), op),
             f = lookup(s.getFalseValue(), op);
        if (!t || !f)
          continue;
        fat[s.getResult()] = {
            SelectOp::create(b, loc, t->off.getType(), s.getCondition(), t->off,
                             f->off),
            SelectOp::create(b, loc, t->size.getType(), s.getCondition(),
                             t->size, f->size)};
      } else if (auto pb = dyn_cast<PtrBaseOp>(op)) {
        auto in = lookup(pb.getPtr(), op);
        if (!in)
          continue;
        b.setInsertionPoint(op);
        Value neg = SubOp::create(b, loc, in->off.getType(),
                                  zero(loc, in->off.getType()), in->off);
        Value base = PtrAddOp::create(b, loc, pb.getType(), pb.getPtr(), neg);
        pb.getResult().replaceAllUsesWith(base);
        fat[base] = {zero(loc, in->off.getType()), in->size};
        toErase.push_back(op);
      } else if (auto pd = dyn_cast<PtrDereferenceableOp>(op)) {
        auto in = lookup(pd.getPtr(), op);
        if (!in)
          continue;
        b.setInsertionPoint(op);
        Value null = PtrNullOp::create(b, loc, pd.getPtr().getType());
        Value nonNull =
            PtrCmpOp::create(b, loc, ICmpPredicate::ne, pd.getPtr(), null);
        Value inObj =
            ICmpOp::create(b, loc, ICmpPredicate::ule, in->off, in->size);
        Value room =
            SubOp::create(b, loc, in->size.getType(), in->size, in->off);
        Value fits =
            ICmpOp::create(b, loc, ICmpPredicate::ule, pd.getLen(), room);
        Value r = AndOp::create(b, loc, nonNull.getType(), nonNull, inObj);
        r = AndOp::create(b, loc, r.getType(), r, fits);
        pd.getResult().replaceAllUsesWith(r);
        toErase.push_back(op);
      } else if (auto ld = dyn_cast<LoadOp>(op); ld && isPtr(ld.getResult())) {
        auto m = lookup(ld.getMem(), op);
        if (!m)
          continue;
        Type t = intOf(ld.getResult());
        fat[ld.getResult()] = {
            LoadOp::create(b, loc, t, m->off, ld.getAddr(), ld.getAlignAttr()),
            LoadOp::create(b, loc, t, m->size, ld.getAddr(),
                           ld.getAlignAttr())};
      } else if (isa<MemNondetOp, MemConstOp>(op) &&
                 needsMem(op->getResult(0))) {
        // The initial companions are never read: the abstract semantics
        // stops on a pointer load from cells no pointer was stored to. So
        // they are unconstrained (an all-zero constant array would need
        // lambdas in some solvers, e.g. Yices).
        Value m = op->getResult(0);
        auto memTy = companionType(cast<MemType>(m.getType()));
        fat[m] = {
            MemNondetOp::create(b, loc, memTy, b.getStringAttr("fat.offset"),
                                b.getI64IntegerAttr(nextSite++)),
            MemNondetOp::create(b, loc, memTy, b.getStringAttr("fat.size"),
                                b.getI64IntegerAttr(nextSite++))};
      } else if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(
                     op) &&
                 needsMem(op->getOperand(0))) {
        auto m = lookup(op->getOperand(0), op);
        if (!m)
          continue;
        Value result = op->getResult(0);
        auto memTy = companionType(cast<MemType>(result.getType()));
        Pair next = *m;
        b.setInsertionPoint(op);
        if (auto st = dyn_cast<StoreOp>(op); st && isPtr(st.getValue())) {
          auto v = lookup(st.getValue(), op);
          if (!v)
            continue;
          next.off = StoreOp::create(b, loc, memTy, m->off, st.getAddr(),
                                     v->off, true, st.getAlignAttr());
          next.size = StoreOp::create(b, loc, memTy, m->size, st.getAddr(),
                                      v->size, true, st.getAlignAttr());
        } else if (auto mv = dyn_cast<MemMoveOp>(op)) {
          auto src = lookup(mv.getSrc(), op);
          if (!src)
            continue;
          next.off = MemMoveOp::create(
              b, loc, memTy, m->off, mv.getAddr(), src->off, mv.getSrcAddr(),
              mv.getLen(), true, mv.getAlignAttr(), mv.getSrcAlignAttr());
          next.size = MemMoveOp::create(
              b, loc, memTy, m->size, mv.getAddr(), src->size, mv.getSrcAddr(),
              mv.getLen(), true, mv.getAlignAttr(), mv.getSrcAlignAttr());
        } else if (isa<MemSetOp, MemHavocOp>(op)) {
          Value addr = op->getOperand(1);
          Value len = isa<MemSetOp>(op) ? cast<MemSetOp>(op).getLen()
                                        : cast<MemHavocOp>(op).getLen();
          Value z = zero(loc, memTy.getUnitType());
          IntegerAttr al = isa<MemSetOp>(op) ? cast<MemSetOp>(op).getAlignAttr()
                                             : IntegerAttr();
          next.off =
              MemSetOp::create(b, loc, memTy, m->off, addr, z, len, true, al);
          next.size =
              MemSetOp::create(b, loc, memTy, m->size, addr, z, len, true, al);
        }
        fat[result] = next;
      } else if (auto br = dyn_cast<BranchOpInterface>(op)) {
        for (unsigned i = 0, n = op->getNumSuccessors(); i < n; ++i) {
          Block *dest = op->getSuccessor(i);
          auto it = extraArgsOf.find(dest);
          if (it == extraArgsOf.end())
            continue;
          SuccessorOperands ops = br.getSuccessorOperands(i);
          SmallVector<Value> extra;
          for (unsigned j : it->second)
            if (auto c = lookup(ops[j], op)) {
              extra.push_back(c->off);
              extra.push_back(c->size);
            }
          ops.append(extra);
        }
      } else {
        for (Value r : op->getResults())
          if (isPtr(r)) {
            diag::error("fat.unsupported",
                        "sea-lower-fat-pointers: pointers made by " +
                            op->getName().getStringRef() + " are not supported",
                        loc);
            ok = false;
          }
      }
    }
  }
  for (Block &block : body)
    if (!visited.contains(&block))
      for (Block *succ : block.getSuccessors())
        if (extraArgsOf.count(succ)) {
          diag::error("fat.unreachable",
                      "a pointer or pointer-holding memory flows through an "
                      "unreachable block",
                      block.getTerminator()->getLoc());
          ok = false;
        }
  if (!ok)
    return signalPassFailure();
  for (Operation *op : toErase)
    op->erase();
  program.walk([&](Operation *op) {
    if (isa<PtrBaseOp, PtrDereferenceableOp>(op)) {
      (void)postconditionFailed("sea-lower-fat-pointers", "a fat-pointer query",
                                op);
      signalPassFailure();
    }
  });
}

} // namespace

std::unique_ptr<Pass> createLowerFatPointersPass() {
  return std::make_unique<LowerFatPointersPass>();
}

} // namespace sea
