// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerSeaImp.cpp - seaimp variables -> SSA values ------------------===//
//
// docs/design/SeaImp.md. Two steps. First, every region access is
// expanded in place: `get` the memory, apply the sea op `inplace`, `set`
// the result (seaimp.load: `get` and load; seaimp.alloca: sea.alloca, the
// region dropped). Then every seaimp.var becomes
// SSA values by MLIR's Mem2Reg (tryToPromoteMemorySlots,
// mlir/lib/Transforms/Mem2Reg.cpp): a seaimp.get is replaced by the value of
// the last seaimp.set on its path, joins get block arguments (the iterated
// dominance frontier of the sets), and a get that no set reaches reads the
// variable's initial value, one nondet op made at the start of the entry block
// from the variable's name and site (sea.nondet / sea.struct.nondet /
// sea.mem.nondet; pointers start as sea.ptr.null). The variable owned the site
// (unique per program, sea.program's verifier): the nondet op takes it over,
// made once per variable and only if some get reads it, so the lowered
// program's sites stay unique.
//
// Contract.
//   Accepts: any verified program. The seaimp verifier admits only
//     variables Mem2Reg promotes (SeaImpOps.cpp): the handle is used only by
//     get / set in the variable's region, the variable is in the entry
//     block.
//   Produces: no seaimp op (checked: sea-lower-seaimp.postcondition; a
//     refusal of Mem2Reg on a verified program is a bug, reported, never a
//     half-lowered program). Programs without seaimp ops are unchanged.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/SeaImp/SeaImpOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/Mem2Reg.h"

using namespace mlir;

namespace sea {
namespace {

class LowerSeaImpPass
    : public PassWrapper<LowerSeaImpPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerSeaImpPass)

  StringRef getArgument() const final { return "sea-lower-seaimp"; }
  StringRef getDescription() const final {
    return "Lower seaimp variables to SSA values (MLIR Mem2Reg); a read "
           "before any write is the variable's nondet initial value";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<SeaDialect, imp::SeaImpDialect>();
  }

  void runOnOperation() override;
};

/// Expands each region access into get / sea op `inplace` / set, before
/// Mem2Reg (which then sees only get and set). Local: no dataflow.
static void expandRegionAccesses(ProgramOp program) {
  SmallVector<Operation *> accesses;
  program.walk([&](Operation *op) {
    if (isa<imp::LoadOp, imp::StoreOp, imp::MemSetOp, imp::MemMoveOp,
            imp::MemHavocOp, imp::AllocaOp>(op))
      accesses.push_back(op);
  });
  for (Operation *op : accesses) {
    OpBuilder b(op);
    Location loc = op->getLoc();
    auto get = [&](Value var) {
      Type t = cast<imp::VarType>(var.getType()).getValueType();
      return imp::GetOp::create(b, loc, t, var).getResult();
    };
    UnitAttr inplace = b.getUnitAttr();
    if (auto ld = dyn_cast<imp::LoadOp>(op)) {
      Value m = get(ld.getVar());
      Value r = LoadOp::create(b, loc, ld.getType(), m, ld.getAddr(),
                               ld.getAlignAttr());
      ld.getResult().replaceAllUsesWith(r);
    } else if (auto st = dyn_cast<imp::StoreOp>(op)) {
      Value m = get(st.getVar());
      Value r = StoreOp::create(b, loc, m.getType(), m, st.getAddr(),
                                st.getValue(), inplace, st.getAlignAttr(),
                                /*tracked=*/UnitAttr());
      imp::SetOp::create(b, loc, st.getVar(), r);
    } else if (auto ms = dyn_cast<imp::MemSetOp>(op)) {
      Value m = get(ms.getVar());
      Value r = MemSetOp::create(b, loc, m.getType(), m, ms.getAddr(),
                                 ms.getValue(), ms.getLen(), inplace,
                                 ms.getAlignAttr(), /*tracked=*/UnitAttr());
      imp::SetOp::create(b, loc, ms.getVar(), r);
    } else if (auto mv = dyn_cast<imp::MemMoveOp>(op)) {
      Value m = get(mv.getVar());
      Value s = mv.getSrc() == mv.getVar() ? m : get(mv.getSrc());
      Value r = MemMoveOp::create(b, loc, m.getType(), m, mv.getAddr(), s,
                                  mv.getSrcAddr(), mv.getLen(), inplace,
                                  mv.getAlignAttr(), mv.getSrcAlignAttr(),
                                  /*tracked=*/UnitAttr());
      imp::SetOp::create(b, loc, mv.getVar(), r);
    } else if (auto mh = dyn_cast<imp::MemHavocOp>(op)) {
      Value m = get(mh.getVar());
      Value r = MemHavocOp::create(b, loc, m.getType(), m, mh.getAddr(),
                                   mh.getLen(), mh.getSiteAttr(), inplace,
                                   /*tracked=*/UnitAttr());
      imp::SetOp::create(b, loc, mh.getVar(), r);
    } else if (auto al = dyn_cast<imp::AllocaOp>(op)) {
      // sea allocation takes no memory: the region is dropped
      Value r = AllocaOp::create(b, loc, al.getType(), al.getSize(),
                                 al.getAlignAttr(), al.getSiteAttr(),
                                 al.getLabelAttr());
      al.getResult().replaceAllUsesWith(r);
    }
    op->erase();
  }
}

void LowerSeaImpPass::runOnOperation() {
  ProgramOp program = getOperation();
  expandRegionAccesses(program);
  SmallVector<PromotableAllocationOpInterface> vars;
  bool anyVar = false;
  SmallVector<imp::VarOp> unused;
  program.walk([&](imp::VarOp v) {
    anyVar = true;
    // upstream tryToPromoteMemorySlots skips slots with no uses
    if (v->use_empty())
      unused.push_back(v);
    else
      vars.push_back(v);
  });
  for (imp::VarOp v : unused)
    v->erase();
  if (anyVar) {
    Block &entry = program.getBody().front();
    OpBuilder builder(&entry, entry.begin());
    DominanceInfo &dominance = getAnalysis<DominanceInfo>();
    DataLayout dataLayout = DataLayout::closest(program);
    // failure = nothing promoted; the postcondition below tells
    (void)tryToPromoteMemorySlots(vars, builder, dataLayout, dominance);
  } else {
    markAllAnalysesPreserved();
  }
  if (Operation *left = imp::findSeaImpOp(program)) {
    (void)postconditionFailed("sea-lower-seaimp", "a seaimp op", left);
    signalPassFailure();
  }
}

} // namespace

std::unique_ptr<Pass> createLowerSeaImpPass() {
  return std::make_unique<LowerSeaImpPass>();
}

} // namespace sea
