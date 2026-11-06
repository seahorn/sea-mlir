// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- WrapFlags.cpp - nsw / nuw promises into assumes or asserts -*- C++ -*-//
//
// add / sub / mul / shl may carry LLVM's nsw / nuw flags: promises that the
// signed / unsigned result fits. Their value is always the wrapped one; this
// pass decides what the promise means for verification:
//   mode=assume  sea.assume "no overflow" (executions that break it are
//                infeasible: LLVM's poison is assumed not to happen);
//   mode=assert  sea.assert "no overflow" (overflow is checked);
//   mode=drop    the promise is forgotten (SeaHorn's behavior).
// In every mode the flags are removed. Overflow is expressed with the
// overflow ops (sea.saddo, ...), which sea-expand-intrinsics lowers; shl
// has no SMT-LIB overflow op and is checked by shifting back.
//
// Contract.
//   Accepts: any program. Produces: no nsw / nuw attribute (checked); in
//   assume / assert mode, one check per flag, inserted right before the
//   flagged op (mode=assert output has several asserts: not BMC form).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

class WrapFlagsPass
    : public PassWrapper<WrapFlagsPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WrapFlagsPass)

  WrapFlagsPass() = default;
  WrapFlagsPass(const WrapFlagsPass &other) : PassWrapper(other) {}

  StringRef getArgument() const final { return "sea-wrap-flags"; }
  StringRef getDescription() const final {
    return "Turn nsw / nuw promises into sea.assume or sea.assert of no "
           "overflow, or drop them";
  }

  void runOnOperation() override;

  Option<std::string> mode{*this, "mode",
                           llvm::cl::desc("assume, assert or drop"),
                           llvm::cl::init("assume")};
};

/// 1 iff shifting x left by s loses bits (unsigned: non-zero bits; signed:
/// bits that disagree with the sign): shifting back does not give x.
static Value shlOverflows(OpBuilder &b, Location loc, bool isSigned, Value x,
                          Value s) {
  Value r = ShlOp::create(b, loc, x.getType(), x, s);
  Value back = isSigned ? Value(AShrOp::create(b, loc, x.getType(), r, s))
                        : Value(LShrOp::create(b, loc, x.getType(), r, s));
  return ICmpOp::create(b, loc, ICmpPredicate::ne, back, x);
}

/// 1 iff the op's signed (isSigned) or unsigned result overflows.
static Value overflows(OpBuilder &b, Location loc, Operation *op,
                       bool isSigned) {
  Value x = op->getOperand(0), y = op->getOperand(1);
  if (isa<AddOp>(op))
    return isSigned ? Value(SAddOOp::create(b, loc, x, y))
                    : Value(UAddOOp::create(b, loc, x, y));
  if (isa<SubOp>(op))
    return isSigned ? Value(SSubOOp::create(b, loc, x, y))
                    : Value(USubOOp::create(b, loc, x, y));
  if (isa<MulOp>(op))
    return isSigned ? Value(SMulOOp::create(b, loc, x, y))
                    : Value(UMulOOp::create(b, loc, x, y));
  return shlOverflows(b, loc, isSigned, x, y);
}

void WrapFlagsPass::runOnOperation() {
  if (mode != "assume" && mode != "assert" && mode != "drop") {
    getOperation().emitError("sea-wrap-flags: mode must be assume, assert or "
                             "drop");
    return signalPassFailure();
  }
  OpBuilder b(&getContext());
  getOperation().walk([&](Operation *op) {
    if (!isa<AddOp, SubOp, MulOp, ShlOp>(op))
      return;
    for (StringRef flag : {"nsw", "nuw"}) {
      if (!op->hasAttr(flag))
        continue;
      op->removeAttr(flag);
      if (mode == "drop")
        continue;
      Location loc = op->getLoc();
      b.setInsertionPoint(op);
      Value ov = overflows(b, loc, op, flag == "nsw");
      Value one = ConstOp::create(b, loc, b.getI1Type(),
                                  IntegerAttr::get(b.getI1Type(), APInt(1, 1)));
      Value ok = XorOp::create(b, loc, b.getI1Type(), ov, one);
      if (mode == "assume")
        AssumeOp::create(b, loc, ok);
      else
        AssertOp::create(b, loc, ok);
    }
  });
  getOperation().walk([&](Operation *op) {
    if (op->hasAttr("nsw") || op->hasAttr("nuw")) {
      (void)postconditionFailed("sea-wrap-flags", "a no-wrap flag", op);
      signalPassFailure();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
}

} // namespace

std::unique_ptr<Pass> createWrapFlagsPass() {
  return std::make_unique<WrapFlagsPass>();
}

} // namespace sea
