// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerTracking.cpp - modification tracking -> shadow memory -*- C++
//-*-===//
//
// At the abstract level a memory value carries the set of objects that
// `tracked` updates wrote (sea.mem.is_modified / reset_modified). This pass
// makes that set an explicit shadow memory: next to every memory version
// that needs it runs a companion version of type !sea.mem<!sea.ptr<W>, i8>,
// keyed by the object's base (sea.ptr.base): 1 = modified. It starts as
// sea.mem.const 0 (tracking starts with an empty set), follows the data
// memory through block arguments, and is written only where the data
// memory's set changes:
//   tracked store at p           -> shadow[base(p)] := 1
//   tracked memset/memmove/
//     memhavoc of n bytes at p   -> shadow[base(p)] := n != 0 ? 1 : old
//   reset_modified p             -> shadow[base(p)] := 0 (data unchanged)
//   is_modified p                -> shadow[base(p)] != 0
// The shadow is a separate chain (metadata about locations, not data that
// travels with the bytes): a memmove marks its destination, it does not
// copy the source's shadow.
//
// Object identity, not data access (docs/design/SeaM-null.md, "Shadow
// memories"). A shadow access is keyed by the object, not by a byte of it:
// its address is sea.ptr.base(p), the base of p's object (offset 0 by
// construction: sea-lower-fat-pointers makes it p - off(p)), also for an
// object of size 0 (null, malloc(0)), which holds no data byte but has a
// shadow cell. So the shadow start is `sea.mem.const 0 shadow`: the VC reads
// every memory that starts there as a shadow memory and applies no A-INB
// rule to it (a shadow access is not a program access; an application
// bounds check says nothing about it); two different objects are two cells
// because placement gives every object its own base (sea-place-static
// reserves at least one unit for an object of size 0).
//
// Contract.
//   Accepts: abstract programs (pointer-indexed memories) after
//     sea-apply-tracking; tracked memories must not flow through
//     unreachable blocks (tracking.unreachable).
//   Produces: no sea.mem.is_modified / reset_modified and no `tracked`
//     update (checked); data memory unchanged; new shadow chains of plain
//     memory ops (sea.ptr.base keys need fat pointers to be placed), each
//     starting at a `sea.mem.const 0 shadow` and accessed (load / store of
//     one i8 cell) only at sea.ptr.base keys.
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
namespace {

class LowerTrackingPass
    : public PassWrapper<LowerTrackingPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerTrackingPass)

  StringRef getArgument() const final { return "sea-lower-tracking"; }
  StringRef getDescription() const final {
    return "Make modification tracking an explicit shadow memory keyed by "
           "object base";
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

static bool isTrackingUse(Operation *op) {
  return isa<IsModifiedOp, ResetModifiedOp>(op) || op->hasAttr("tracked");
}

void LowerTrackingPass::runOnOperation() {
  ProgramOp program = getOperation();
  parent.clear();
  auto isMem = [](Value v) { return isa<MemType>(v.getType()); };

  // Memories (def-use classes, as in sea-widen-memory) that need a shadow.
  bool any = false;
  program.walk([&](Operation *op) {
    any = any || isTrackingUse(op);
    if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(op))
      unite(op->getOperand(0), op->getResult(0));
    if (auto br = dyn_cast<BranchOpInterface>(op))
      for (unsigned i = 0, n = op->getNumSuccessors(); i < n; ++i) {
        SuccessorOperands ops = br.getSuccessorOperands(i);
        for (unsigned j = 0, m = ops.size(); j < m; ++j)
          if (Value in = ops[j]; in && isMem(in))
            unite(in, op->getSuccessor(i)->getArgument(j));
      }
  });
  if (!any)
    return;
  llvm::DenseSet<Value> tracked; // class leaders
  program.walk([&](Operation *op) {
    if (isTrackingUse(op))
      tracked.insert(find(op->getOperand(0)));
  });
  auto needs = [&](Value v) { return isMem(v) && tracked.contains(find(v)); };

  // Abstract memories only: the key is an object base.
  bool ok = true;
  for (Value leader : tracked) {
    auto t = cast<MemType>(leader.getType());
    if (!isa<PtrType>(t.getIndex())) {
      diag::error("tracking.placed",
                  "modification tracking is lowered on abstract (pointer-"
                  "indexed) memory only",
                  leader.getLoc());
      ok = false;
    }
  }
  if (!ok)
    return signalPassFailure();

  MLIRContext *ctx = &getContext();
  OpBuilder b(ctx);
  auto i8 = IntegerType::get(ctx, 8);
  auto shadowType = [&](Value data) {
    return MemType::get(ctx, cast<MemType>(data.getType()).getIndex(), i8);
  };
  llvm::DenseMap<Value, Value> shadow; // data version -> shadow version

  // Shadow block arguments first (appended, in the order of their data
  // arguments), so branches can forward to them in any visiting order.
  Region &body = program.getBody();
  llvm::DenseMap<Block *, SmallVector<unsigned>> shadowArgsOf;
  for (Block &block : body) {
    unsigned n = block.getNumArguments();
    for (unsigned j = 0; j < n; ++j) {
      BlockArgument a = block.getArgument(j);
      if (!needs(a))
        continue;
      shadow[a] = block.addArgument(shadowType(a), a.getLoc());
      shadowArgsOf[&block].push_back(j);
    }
  }

  llvm::DenseSet<Block *> visited;
  SmallVector<std::pair<Value, Value>> forward; // reset result -> data in
  SmallVector<Operation *> toErase;
  auto cst = [&](Location loc, Type t, uint64_t v) -> Value {
    return ConstOp::create(
        b, loc, t, IntegerAttr::get(t, APInt(t.getIntOrFloatBitWidth(), v)));
  };
  for (Block *block : llvm::ReversePostOrderTraversal<Region *>(&body)) {
    visited.insert(block);
    for (Operation &opRef : llvm::make_early_inc_range(*block)) {
      Operation *op = &opRef;
      Location loc = op->getLoc();
      if (isa<MemNondetOp, MemConstOp>(op) && needs(op->getResult(0))) {
        b.setInsertionPointAfter(op);
        auto start = MemConstOp::create(b, loc, shadowType(op->getResult(0)),
                                        cst(loc, i8, 0));
        start.setShadow(true);
        shadow[op->getResult(0)] = start;
        continue;
      }
      if (auto im = dyn_cast<IsModifiedOp>(op)) {
        b.setInsertionPoint(op);
        Value base =
            PtrBaseOp::create(b, loc, im.getPtr().getType(), im.getPtr());
        Value v = LoadOp::create(b, loc, i8, shadow.lookup(im.getMem()), base,
                                 IntegerAttr());
        Value r = ICmpOp::create(b, loc, ICmpPredicate::ne, v, cst(loc, i8, 0));
        im.getResult().replaceAllUsesWith(r);
        toErase.push_back(op);
        continue;
      }
      if (isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(op) &&
          needs(op->getOperand(0))) {
        Value s = shadow.lookup(op->getOperand(0));
        Value data = op->getResult(0);
        if (!op->hasAttr("tracked") && !isa<ResetModifiedOp>(op)) {
          shadow[data] = s;
          continue;
        }
        b.setInsertionPoint(op);
        Value base = PtrBaseOp::create(b, loc, op->getOperand(1).getType(),
                                       op->getOperand(1));
        Value mark;
        if (isa<ResetModifiedOp>(op)) {
          mark = cst(loc, i8, 0);
        } else if (isa<StoreOp>(op)) {
          mark = cst(loc, i8, 1);
        } else {
          Value len = isa<MemSetOp>(op)    ? cast<MemSetOp>(op).getLen()
                      : isa<MemMoveOp>(op) ? cast<MemMoveOp>(op).getLen()
                                           : cast<MemHavocOp>(op).getLen();
          Value old = LoadOp::create(b, loc, i8, s, base, IntegerAttr());
          Value wrote = ICmpOp::create(b, loc, ICmpPredicate::ne, len,
                                       cst(loc, len.getType(), 0));
          mark = SelectOp::create(b, loc, wrote, cst(loc, i8, 1), old);
        }
        shadow[data] = StoreOp::create(b, loc, s.getType(), s, base, mark,
                                       /*inplace=*/true, IntegerAttr());
        if (isa<ResetModifiedOp>(op)) {
          forward.push_back({data, op->getOperand(0)});
          toErase.push_back(op);
        } else {
          op->removeAttr("tracked");
        }
        continue;
      }
      if (auto br = dyn_cast<BranchOpInterface>(op))
        for (unsigned i = 0, n = op->getNumSuccessors(); i < n; ++i) {
          Block *dest = op->getSuccessor(i);
          auto it = shadowArgsOf.find(dest);
          if (it == shadowArgsOf.end())
            continue;
          SuccessorOperands ops = br.getSuccessorOperands(i);
          SmallVector<Value> extra;
          for (unsigned j : it->second)
            extra.push_back(shadow.lookup(ops[j]));
          ops.append(extra);
        }
    }
  }
  for (Block &block : body)
    if (!visited.contains(&block))
      for (Block *succ : block.getSuccessors())
        if (shadowArgsOf.count(succ)) {
          diag::error("tracking.unreachable",
                      "a tracked memory flows through an unreachable block",
                      block.getTerminator()->getLoc());
          return signalPassFailure();
        }

  // latest first: a reset of a reset forwards to the first one's input
  for (auto [result, in] : llvm::reverse(forward))
    result.replaceAllUsesWith(in);
  for (Operation *op : toErase)
    op->erase();

  program.walk([&](Operation *op) {
    if (isTrackingUse(op)) {
      (void)postconditionFailed("sea-lower-tracking", "modification tracking",
                                op);
      signalPassFailure();
    }
  });
}

} // namespace

std::unique_ptr<Pass> createLowerTrackingPass() {
  return std::make_unique<LowerTrackingPass>();
}

} // namespace sea
