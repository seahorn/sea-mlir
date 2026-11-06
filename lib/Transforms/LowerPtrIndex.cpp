// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerPtrIndex.cpp - sea.ptr.index -> mul / add / ptr.add ----------===//
//
// docs/design/PtrIndex.md. `sea.ptr.index %p [%i_1..%i_m] strides [s..]
// offset c` becomes plain arithmetic, for consumers that do not use the op:
//     %o = sext(i_1)*s_1 + ... + sext(i_m)*s_m + c     (n bits, modular)
//     %q = ptr.add %p, %o    (addr.add for an address base)
// The value is unchanged. The no-wrap flags are dropped, not mapped onto
// nsw / nuw of the new ops: the promise is order-free (P1-P3: products, the
// total offset, base + offset), while flags on these adds would promise
// every partial sum in this order, which the front end did not give.
// Dropping a flag loses precision only. The flags are counted
// (`lower-ptr-index.dropped-flags`).
//
// Contract.
//   Accepts: any program.
//   Produces: no sea.ptr.index (checked: lower-ptr-index.postcondition).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/Sea/SeaTypes.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class LowerPtrIndexPass
    : public PassWrapper<LowerPtrIndexPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerPtrIndexPass)

  StringRef getArgument() const final { return "sea-lower-ptr-index"; }
  StringRef getDescription() const final {
    return "Lower sea.ptr.index to mul / add / ptr.add (flags dropped)";
  }

  void runOnOperation() override;
};

void LowerPtrIndexPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<PtrIndexOp> ops;
  program.walk([&](PtrIndexOp p) { ops.push_back(p); });
  OpBuilder b(&getContext());
  uint64_t dropped = 0;
  for (PtrIndexOp p : ops) {
    b.setInsertionPoint(p);
    Location loc = p.getLoc();
    Value off = ptrIndexOffsetValue(b, p);
    if (p.getInbounds() || p.getNusw() || p.getNuw())
      ++dropped;
    p.replaceAllUsesWith(offsetIndex(b, loc, p.getBase(), off));
    p.erase();
  }
  if (dropped)
    diag::warn("lower-ptr-index.dropped-flags",
               Twine(dropped) + " sea.ptr.index no-wrap flag set(s) dropped by "
                                "sea-lower-ptr-index (precision only)");
  bool left = false;
  program.walk([&](PtrIndexOp) { left = true; });
  if (left) {
    diag::error("lower-ptr-index.postcondition",
                "a sea.ptr.index is left: a bug in sea-lower-ptr-index");
    signalPassFailure();
  }
}

} // namespace

std::unique_ptr<Pass> createLowerPtrIndexPass() {
  return std::make_unique<LowerPtrIndexPass>();
}

} // namespace sea
