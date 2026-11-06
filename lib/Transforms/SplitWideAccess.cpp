// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SplitWideAccess.cpp - wide loads/stores into cell accesses -*- C++ -*-//
//
// A load or store whose value spans n > 1 cells becomes n cell accesses at
// index + i (little-endian) joined with sea.concat / split with sea.extract.
// The VC only handles single-cell accesses; the interpreter handles both,
// which is what lets the fuzzer validate this pass.
//
// Contract.
//   Accepts: accesses of a multiple of the cell width (the verifier's
//     rule), integer or address values; pointer values only after
//     placement.
//   Produces: every load / store moves exactly one cell (checked); the
//     `inplace` promise of a split store carries over to each cell store.
//   Errors: split.pointer-value.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"
#include "sea/Transforms/WidthDomain.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

class SplitWideAccessPass
    : public PassWrapper<SplitWideAccessPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitWideAccessPass)

  StringRef getArgument() const final { return "sea-split-wide-access"; }
  StringRef getDescription() const final {
    return "Split loads and stores that span several cells into cell "
           "accesses (little-endian) with sea.concat / sea.extract";
  }

  SplitWideAccessPass() = default;
  SplitWideAccessPass(const SplitWideAccessPass &other) : PassWrapper(other) {}

  void runOnOperation() override;

  Option<bool> keepByteWide{
      *this, "keep-byte-wide",
      llvm::cl::desc("Leave whole the wide accesses of exact byte memories "
                     "(i8 cells, byte addresses) whose width is within the "
                     "address space: sea-vc --seam --seam-wide encodes them "
                     "as width terms (docs/design/SeaM-mixed-width.md)"),
      llvm::cl::init(false)};

private:
  /// a wide access kept whole under keep-byte-wide
  bool keptWhole(MemType memTy, unsigned width) const {
    // the width domain (WidthDomain.h); outside it the access is split
    return keepByteWide && inWideByteDomain(memTy, width);
  }
};

void SplitWideAccessPass::runOnOperation() {
  ProgramOp program = getOperation();
  OpBuilder b(&getContext());
  bool ok = true;
  SmallVector<Operation *> toErase;
  program.walk([&](Operation *op) {
    auto load = dyn_cast<LoadOp>(op);
    auto store = dyn_cast<StoreOp>(op);
    if (!load && !store)
      return;
    MemType memTy = cast<MemType>(op->getOperand(0).getType());
    unsigned cell = memTy.getCell().getWidth();
    unsigned units = memTy.getCellUnits(); // index units per cell
    Type valTy = load ? load.getType() : store.getValue().getType();
    unsigned width = bitWidthOf(valTy);
    if (width == cell || keptWhole(memTy, width))
      return;
    if (isa<PtrType>(valTy)) {
      diag::error("split.pointer-value",
                  "sea-split-wide-access: a pointer value cannot be split "
                  "into cells before placement (run sea-place-static first)",
                  op->getLoc());
      ok = false;
      return;
    }
    unsigned n = width / cell;
    auto iC = IntegerType::get(&getContext(), cell);
    auto iW = IntegerType::get(&getContext(), width);
    Location loc = op->getLoc();
    b.setInsertionPoint(op);
    if (load) {
      Value acc;
      for (unsigned i = 0; i < n; ++i) {
        Value at = offsetIndex(b, loc, load.getAddr(), i * units);
        // The cell accesses carry no align: dropping a promise is safe.
        Value c = LoadOp::create(b, loc, iC, load.getMem(), at, IntegerAttr());
        if (i == 0) {
          acc = c;
          continue;
        }
        auto accTy = IntegerType::get(&getContext(), cell * (i + 1));
        acc = ConcatOp::create(b, loc, accTy, c, acc);
      }
      if (isa<AddrType>(valTy))
        // the pointer's own bits read back: not an int-to-ptr (D2)
        acc = AddrFromBitsOp::create(b, loc, valTy, acc);
      load.getResult().replaceAllUsesWith(acc);
    } else {
      Value v = store.getValue();
      if (isa<AddrType>(valTy))
        v = AddrToBitsOp::create(b, loc, iW, v);
      Value mem = store.getMem();
      for (unsigned i = 0; i < n; ++i) {
        Value at = offsetIndex(b, loc, store.getAddr(), i * units);
        Value part =
            ExtractOp::create(b, loc, iC, v, b.getI64IntegerAttr(i * cell));
        mem = StoreOp::create(b, loc, memTy, mem, at, part, store.getInplace(),
                              IntegerAttr());
      }
      store.getResult().replaceAllUsesWith(mem);
    }
    toErase.push_back(op);
  });
  for (Operation *op : toErase)
    op->erase();
  if (!ok)
    return signalPassFailure();
  program.walk([&](Operation *op) {
    if (!isa<LoadOp, StoreOp>(op) || !ok)
      return;
    Type valTy = isa<LoadOp>(op) ? op->getResult(0).getType()
                                 : op->getOperand(2).getType();
    MemType memTy = cast<MemType>(op->getOperand(0).getType());
    if (bitWidthOf(valTy) != memTy.getCell().getWidth() &&
        !keptWhole(memTy, bitWidthOf(valTy)))
      ok = succeeded(postconditionFailed("sea-split-wide-access",
                                         "a multi-cell access", op));
  });
  if (!ok)
    signalPassFailure();
}

} // namespace

std::unique_ptr<Pass> createSplitWideAccessPass(bool keepByteWide) {
  auto p = std::make_unique<SplitWideAccessPass>();
  p->keepByteWide = keepByteWide;
  return p;
}

} // namespace sea
