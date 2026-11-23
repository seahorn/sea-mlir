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
// Option keep-bytes (sea-vc --seam): the fills of byte memories
// (MemType::isByteCells: i8 cells of one unit, not keyed) are kept, SeaM
// encodes each as one term (mfill, docs/design/MemFill.md "SeaM"); the
// others (word layouts, keyed memories) are expanded as without it.
//
// Contract.
//   Accepts: any program.
//   Produces: no sea.mem.fill, or with keep-bytes none but of byte memories
//     (checked: sea-expand-mem-fill.postcondition).
//   Reports: the counters (kExpandMemFillStatsAttr on the program, when it
//     has a fill; sea-vc prints vc.expand-mem-fill): expanded, cells (stores
//     made), kept (keep-bytes), and the expanded fills by why they were not
//     kept under keep-bytes: expanded-word-layout, expanded-keyed,
//     expanded-other (cells that are not i8).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class ExpandMemFillPass
    : public PassWrapper<ExpandMemFillPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ExpandMemFillPass)

  ExpandMemFillPass() = default;
  ExpandMemFillPass(const ExpandMemFillPass &other) : PassWrapper(other) {}
  explicit ExpandMemFillPass(bool keep) { keepBytes = keep; }

  StringRef getArgument() const final { return "sea-expand-mem-fill"; }
  StringRef getDescription() const final {
    return "Expand each sea.mem.fill into one store per cell (of the "
           "memory's cell type), in order";
  }

  void runOnOperation() override;

  Option<bool> keepBytes{
      *this, "keep-bytes",
      llvm::cl::desc("Keep the fills of byte memories (i8 cells, not keyed): "
                     "SeaM encodes them as terms (sea-vc --seam)"),
      llvm::cl::init(false)};
};

void ExpandMemFillPass::runOnOperation() {
  ProgramOp program = getOperation();
  SmallVector<MemFillOp> fills;
  program.walk([&](MemFillOp op) { fills.push_back(op); });
  if (fills.empty())
    return markAllAnalysesPreserved();
  struct Counts {
    uint64_t expanded = 0, cells = 0, kept = 0, words = 0, keyed = 0, other = 0;
  } n;
  for (MemFillOp op : fills) {
    MemType ty = op.getMem().getType();
    if (keepBytes) {
      if (ty.isByteCells()) {
        ++n.kept;
        continue;
      }
      ++(ty.isWordLayout() ? n.words : ty.isKeyed() ? n.keyed : n.other);
    }
    ++n.expanded;
    n.cells += uint64_t(op.getData().getNumElements());
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
  MLIRContext *ctx = &getContext();
  auto stat = [&](StringRef key, uint64_t v) {
    return NamedAttribute(StringAttr::get(ctx, key),
                          IntegerAttr::get(IntegerType::get(ctx, 64), v));
  };
  SmallVector<NamedAttribute> stats{stat("expanded", n.expanded),
                                    stat("cells", n.cells),
                                    stat("kept", n.kept),
                                    stat("expanded-word-layout", n.words),
                                    stat("expanded-keyed", n.keyed),
                                    stat("expanded-other", n.other)};
  program->setAttr(kExpandMemFillStatsAttr, DictionaryAttr::get(ctx, stats));
  bool keep = keepBytes;
  WalkResult left = program.walk([&](MemFillOp op) {
    if (keep && op.getMem().getType().isByteCells())
      return WalkResult::advance();
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

std::unique_ptr<Pass> createExpandMemFillPass(bool keepBytes) {
  return std::make_unique<ExpandMemFillPass>(keepBytes);
}

} // namespace sea
