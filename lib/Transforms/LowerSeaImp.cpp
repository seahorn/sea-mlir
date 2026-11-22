// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerSeaImp.cpp - seaimp variables -> SSA values ------------------===//
//
// docs/design/SeaImp.md. Two steps. First, every region access is
// expanded in place: `get` the memory, apply the sea op `inplace`, `set`
// the result (the reads seaimp.load / is_modified / print.mem: `get` and
// the sea op; a read in a sea.box.all body: its `get` before the box;
// seaimp.alloca / malloc / global: the sea op, the region dropped). These
// get / set of memory exist only inside the pass (the verifier refuses
// them in input): each get's value is used by the one op made with it, and
// each set stores the result of a fresh update, so every version is used
// linearly and `inplace` holds. Then every seaimp.var becomes
// SSA values by MLIR's Mem2Reg (tryToPromoteMemorySlots,
// mlir/lib/Transforms/Mem2Reg.cpp): a seaimp.get is replaced by the value of
// the last seaimp.set on its path, joins get block arguments (the iterated
// dominance frontier of the sets), and a get that no set reaches reads the
// variable's initial value, one nondet op made at the start of the entry block
// from the variable's name and site (sea.nondet / sea.struct.nondet /
// sea.mem.nondet; pointers start as sea.ptr.null; a memory variable with
// `init %v` starts as sea.mem.const %v, made right after the variable, which
// %v dominates, and has no site). The variable owned the site
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
/// Mem2Reg (which then sees only get and set). Local: no dataflow. A read in
/// a sea.box.all body gets its `get` right before the box op, in the
/// variable's region: the body cannot write memory, so the version before
/// the box is the one every index reads, and Mem2Reg sees no use in a
/// nested region.
static void expandRegionAccesses(ProgramOp program) {
  SmallVector<Operation *> accesses;
  program.walk([&](Operation *op) {
    if (isa<imp::LoadOp, imp::StoreOp, imp::MemSetOp, imp::MemMoveOp,
            imp::MemHavocOp, imp::MemFillOp, imp::IsModifiedOp,
            imp::ResetModifiedOp, imp::PrintMemOp, imp::AllocaOp, imp::MallocOp,
            imp::GlobalOp>(op))
      accesses.push_back(op);
  });
  for (Operation *op : accesses) {
    OpBuilder b(op);
    Location loc = op->getLoc();
    // the get: before the op, or before the box whose body holds it (the
    // verifier admits only reads there)
    Operation *top = op;
    if (isa<BoxAllOp>(op->getParentOp()))
      top = op->getParentOp();
    auto get = [&](Value var) {
      Type t = cast<imp::VarType>(var.getType()).getValueType();
      OpBuilder gb(top);
      return imp::GetOp::create(gb, loc, t, var).getResult();
    };
    UnitAttr inplace = b.getUnitAttr();
    if (auto ld = dyn_cast<imp::LoadOp>(op)) {
      Value m = get(ld.getVar());
      Value r = LoadOp::create(b, loc, ld.getType(), m, ld.getAddr(),
                               ld.getAlignAttr());
      ld.getResult().replaceAllUsesWith(r);
    } else if (auto st = dyn_cast<imp::StoreOp>(op)) {
      Value m = get(st.getVar());
      Value r =
          StoreOp::create(b, loc, m.getType(), m, st.getAddr(), st.getValue(),
                          inplace, st.getAlignAttr(), st.getTrackedAttr());
      imp::SetOp::create(b, loc, st.getVar(), r);
    } else if (auto ms = dyn_cast<imp::MemSetOp>(op)) {
      Value m = get(ms.getVar());
      Value r = MemSetOp::create(b, loc, m.getType(), m, ms.getAddr(),
                                 ms.getValue(), ms.getLen(), inplace,
                                 ms.getAlignAttr(), ms.getTrackedAttr());
      imp::SetOp::create(b, loc, ms.getVar(), r);
    } else if (auto mv = dyn_cast<imp::MemMoveOp>(op)) {
      Value m = get(mv.getVar());
      Value s = mv.getSrc() == mv.getVar() ? m : get(mv.getSrc());
      Value r = MemMoveOp::create(b, loc, m.getType(), m, mv.getAddr(), s,
                                  mv.getSrcAddr(), mv.getLen(), inplace,
                                  mv.getAlignAttr(), mv.getSrcAlignAttr(),
                                  mv.getTrackedAttr());
      imp::SetOp::create(b, loc, mv.getVar(), r);
    } else if (auto mh = dyn_cast<imp::MemHavocOp>(op)) {
      Value m = get(mh.getVar());
      Value r =
          MemHavocOp::create(b, loc, m.getType(), m, mh.getAddr(), mh.getLen(),
                             mh.getSiteAttr(), inplace, mh.getTrackedAttr());
      imp::SetOp::create(b, loc, mh.getVar(), r);
    } else if (auto mf = dyn_cast<imp::MemFillOp>(op)) {
      Value m = get(mf.getVar());
      Value r =
          MemFillOp::create(b, loc, m.getType(), m, mf.getAddr(),
                            mf.getDataAttr(), inplace, mf.getTrackedAttr());
      imp::SetOp::create(b, loc, mf.getVar(), r);
    } else if (auto im = dyn_cast<imp::IsModifiedOp>(op)) {
      Value m = get(im.getVar());
      Value r = IsModifiedOp::create(b, loc, b.getI1Type(), m, im.getPtr());
      im.getResult().replaceAllUsesWith(r);
    } else if (auto rm = dyn_cast<imp::ResetModifiedOp>(op)) {
      Value m = get(rm.getVar());
      Value r = ResetModifiedOp::create(b, loc, m.getType(), m, rm.getAddr(),
                                        inplace);
      imp::SetOp::create(b, loc, rm.getVar(), r);
    } else if (auto pm = dyn_cast<imp::PrintMemOp>(op)) {
      Value m = get(pm.getVar());
      SmallVector<Value, 3> args{m, pm.getAddr()};
      if (Value n = pm.getCountValue())
        args.push_back(n);
      PrintMemOp::create(b, loc, args, pm.getTagAttr(), /*mem=*/StringAttr(),
                         /*addr=*/StringAttr(), pm.getTypeAttr(),
                         pm.getFmtAttr(), pm.getCountAttr(),
                         /*count_ref=*/StringAttr(), pm.getOffsetAttr());
    } else if (auto al = dyn_cast<imp::AllocaOp>(op)) {
      // sea allocation takes no memory: the region is dropped
      Value r = AllocaOp::create(b, loc, al.getType(), al.getSize(),
                                 al.getAlignAttr(), al.getSiteAttr(),
                                 al.getLabelAttr());
      al.getResult().replaceAllUsesWith(r);
    } else if (auto ma = dyn_cast<imp::MallocOp>(op)) {
      Value r = MallocOp::create(b, loc, ma.getType(), ma.getSize(),
                                 ma.getAlignAttr(), ma.getSiteAttr(),
                                 ma.getLabelAttr());
      ma.getResult().replaceAllUsesWith(r);
    } else if (auto gl = dyn_cast<imp::GlobalOp>(op)) {
      Value r = GlobalOp::create(b, loc, gl.getType(), gl.getNameAttr(),
                                 gl.getSizeAttr(), gl.getAlignAttr(),
                                 gl.getLabelAttr());
      gl.getResult().replaceAllUsesWith(r);
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
