// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- CheckAlign.cpp - the A-ALIGN discharge job's instrumentation -----===//
//
// docs/design/SeaM-mixed-width.md rev 5: A-ALIGN ("every access with
// `align a` has an address that is a multiple of a") is a named assumption
// of the rewriting (and of sea-widen-memory); it is discharged by a separate
// job, never by verifying without it (conditional soundness). This pass is
// that job's instrumentation: before every load / store / memset / memmove
// (destination and source) with `align a`, a >= 2,
//     sea.assert ((address as an integer) & (a - 1)) == 0
// The program's own asserts are dropped (the job checks alignment on every
// execution, not the functional property); sea-merge-asserts then makes the
// BMC form. Run after placement (addresses exist) and before
// sea-widen-memory / sea-split-wide-access, which rely on or drop the
// promises: the check sees every original promise.
//
// Contract.
//   Accepts: a placed program (pointer values have addresses).
//   Produces: one assert per promised access (counted: `checks`), no
//     original assert.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class CheckAlignPass
    : public PassWrapper<CheckAlignPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckAlignPass)

  StringRef getArgument() const final { return "sea-check-align"; }
  StringRef getDescription() const final {
    return "Assert every `align` promise of a load / store / memset / "
           "memmove (the A-ALIGN discharge job); drop the program's asserts";
  }

  void runOnOperation() override;
};

void CheckAlignPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  OpBuilder b(ctx);
  SmallVector<AssertOp> original;
  program.walk([&](AssertOp a) { original.push_back(a); });
  auto check = [&](Operation *op, Value addr, std::optional<uint64_t> align) {
    if (!align || *align < 2)
      return;
    Location loc = op->getLoc();
    b.setInsertionPoint(op);
    Value i;
    unsigned w;
    if (auto pt = dyn_cast<PtrType>(addr.getType())) {
      w = pt.getWidth();
      i = PtrToIntOp::create(b, loc, IntegerType::get(ctx, w), addr);
    } else if (auto at = dyn_cast<AddrType>(addr.getType())) {
      w = at.getWidth();
      i = AddrToIntOp::create(b, loc, IntegerType::get(ctx, w), addr);
    } else {
      return;
    }
    auto iW = IntegerType::get(ctx, w);
    Value mask =
        ConstOp::create(b, loc, iW, IntegerAttr::get(iW, APInt(w, *align - 1)));
    Value zero = ConstOp::create(b, loc, iW, IntegerAttr::get(iW, APInt(w, 0)));
    Value low = AndOp::create(b, loc, iW, i, mask);
    Value ok = ICmpOp::create(b, loc, ICmpPredicate::eq, low, zero);
    AssertOp::create(b, loc, ok);
  };
  program.walk([&](Operation *op) {
    if (auto ld = dyn_cast<LoadOp>(op))
      check(op, ld.getAddr(), ld.getAlign());
    else if (auto st = dyn_cast<StoreOp>(op))
      check(op, st.getAddr(), st.getAlign());
    else if (auto ms = dyn_cast<MemSetOp>(op))
      check(op, ms.getAddr(), ms.getAlign());
    else if (auto mv = dyn_cast<MemMoveOp>(op)) {
      check(op, mv.getAddr(), mv.getAlign());
      check(op, mv.getSrcAddr(), mv.getSrcAlign());
    }
  });
  OpBuilder mb(&getContext());
  for (AssertOp a : original) {
    // the top of a tagged assert stays a reach target (sea-vc --reach)
    if (Attribute site = a->getAttr(kReachSiteAttr)) {
      mb.setInsertionPoint(a);
      createReachSiteMarker(mb, a.getLoc(), a->getName().getStringRef(), site);
    }
    a.erase();
  }
}

} // namespace

std::unique_ptr<Pass> createCheckAlignPass() {
  return std::make_unique<CheckAlignPass>();
}

} // namespace sea
