// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerSeaImp.cpp - seaimp variables -> SSA values ------------------===//
//
// docs/design/SeaImp.md. Every seaimp.var becomes SSA values by MLIR's
// Mem2Reg (tryToPromoteMemorySlots, mlir/lib/Transforms/Mem2Reg.cpp): a
// seaimp.get is replaced by the value of the last seaimp.set on its path,
// joins get block arguments (the iterated dominance frontier of the sets),
// and a get that no set reaches reads the variable's initial value, one
// nondet op made at the start of the entry block from the variable's name
// and site (sea.nondet / sea.struct.nondet / sea.ptr.nondet). The variable
// owned the site (unique per program, sea.program's verifier): the nondet
// op takes it over, made once per variable and only if some get reads it,
// so the lowered program's sites stay unique.
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

void LowerSeaImpPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<PromotableAllocationOpInterface> vars;
  program.walk([&](imp::VarOp v) { vars.push_back(v); });
  if (!vars.empty()) {
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
