// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExpandMemFill.cpp - sea.mem.fill -> one store per cell -*- C++ -*-===//
//
// docs/design/MemFill.md. The VC encodes no sea.mem.fill: every memory mode
// (arrays, lambda, word layouts, SeaM) sees stores. A fill of N cells
// becomes N stores, in order, of one cell each (the memory's cell type: a
// byte on a byte memory, a k-byte word on a widened one) at
//   addr + k * c   (c = index units per cell; k = 0 .. N-1)
// of the constant cell k of the data:
//
//   %m1 = sea.mem.fill %m0[%p], dense<[1, 2]> : tensor<2xi8> inplace : !m
// ->
//   %s0 = sea.store %m0[%p], %c1 inplace : !m, i8
//   %q1 = sea.ptr.add %p, %c1_i64 : ...
//   %m1 = sea.store %s0[%q1], %c2 inplace : !m, i8
//
// Exact: the stores write the cells the fill writes, with its values, and
// nothing else. The first store consumes the fill's memory as the fill did
// (its `inplace`); the others consume the version the previous store made,
// which nothing else uses (`inplace`). `tracked` is kept on every store
// (a tracked fill marks the object of `addr`; so do N tracked stores at
// offsets of `addr`, while they stay in that object).
//
// In the VC lowering it runs after sea-widen-memory, which decides a
// memory's cells with the fill still whole (a fill qualifies for k-byte
// words if it starts provably aligned and has a multiple of k bytes; it
// then packs the data into words), so the stores have the final cell type.
//
// Contract.
//   Accepts: any program.
//   Produces: no sea.mem.fill (checked: sea-expand-mem-fill.postcondition).
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

class ExpandMemFillPass
    : public PassWrapper<ExpandMemFillPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ExpandMemFillPass)

  StringRef getArgument() const final { return "sea-expand-mem-fill"; }
  StringRef getDescription() const final {
    return "Expand each sea.mem.fill into one store per cell (of the "
           "memory's cell type), in order";
  }

  void runOnOperation() override;
};

void ExpandMemFillPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<MemFillOp> fills;
  program.walk([&](MemFillOp op) { fills.push_back(op); });
  if (fills.empty())
    return markAllAnalysesPreserved();
  for (MemFillOp op : fills) {
    OpBuilder b(op);
    Location loc = op.getLoc();
    MemType memTy = op.getMem().getType();
    IntegerType cellTy = memTy.getCell();
    unsigned units = memTy.getCellUnits();
    // one constant per distinct cell value (initializers repeat values)
    llvm::DenseMap<APInt, Value> constants;
    auto cell = [&](const APInt &v) {
      Value &c = constants[v];
      if (!c)
        c = ConstOp::create(b, loc, cellTy, IntegerAttr::get(cellTy, v));
      return c;
    };
    Value mem = op.getMem();
    bool inplace = op.getInplace();
    for (auto [k, v] : llvm::enumerate(op.getData().getValues<APInt>())) {
      Value at = offsetIndex(b, loc, op.getAddr(), uint64_t(k) * units);
      mem = StoreOp::create(b, loc, memTy, mem, at, cell(v), inplace,
                            IntegerAttr(), op.getTracked());
      inplace = true; // a version only this chain uses
    }
    op.getResult().replaceAllUsesWith(mem);
    op.erase();
  }
  WalkResult left = program.walk([](MemFillOp op) {
    (void)postconditionFailed("sea-expand-mem-fill", "a sea.mem.fill", op);
    return WalkResult::interrupt();
  });
  if (left.wasInterrupted())
    signalPassFailure();
}

} // namespace

std::unique_ptr<Pass> createExpandMemFillPass() {
  return std::make_unique<ExpandMemFillPass>();
}

} // namespace sea
