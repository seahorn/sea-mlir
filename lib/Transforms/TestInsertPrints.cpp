// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- TestInsertPrints.cpp - prints everywhere (a test pass) ---*- C++ -*-===//
//
// sea-test-insert-prints{mode=weak|strong}: a print after every op that
// defines one value, a sea.print.mem after every memory version, a print
// scope around every block's body and a tag line right before every
// sea.halt (so between a BMC-form assert and its halt). For the tests that
// prints change nothing (docs/design/Print.md): the VC of a program is the
// same with and without them, and every pass accepts them.
//   weak: names (the value's print name, or an unknown one: dangling);
//   strong: operands, for values that already have a use (so liveness is
//     the program's own) and memories with an address of their index type
//     defined before in the block;
//   strong-all: operands for every value (prints keep dead values alive:
//     what a strong print costs the VC).
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class TestInsertPrintsPass
    : public PassWrapper<TestInsertPrintsPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TestInsertPrintsPass)
  TestInsertPrintsPass() = default;
  TestInsertPrintsPass(const TestInsertPrintsPass &o) : PassWrapper(o) {}
  StringRef getArgument() const final { return "sea-test-insert-prints"; }
  StringRef getDescription() const final {
    return "Test only: insert print ops everywhere (weak or strong)";
  }
  void runOnOperation() override;
  Option<std::string> mode{*this, "mode",
                           llvm::cl::desc("weak, strong or strong-all"),
                           llvm::cl::init("weak")};
};

void TestInsertPrintsPass::runOnOperation() {
  bool all = mode == "strong-all";
  bool strong = mode == "strong" || all;
  if (!strong && mode != "weak") {
    getOperation().emitError("sea-test-insert-prints: mode must be weak or "
                             "strong");
    return signalPassFailure();
  }
  OpBuilder b(&getContext());
  unsigned n = 0;
  auto name = [&](Value v) {
    std::optional<std::string> s = printNameOf(v);
    return s ? *s : "v" + std::to_string(n);
  };
  for (Block &block : getOperation().getBody()) {
    Location loc = block.getParent()->getLoc();
    b.setInsertionPointToStart(&block);
    PrintScopeOp::create(b, loc, b.getStringAttr("bb"));
    SmallVector<Operation *> ops;
    for (Operation &op : block.without_terminator())
      if (!isPrintOp(&op))
        ops.push_back(&op);
    for (Operation *op : ops) {
      if (op->getNumResults() != 1)
        continue;
      Value v = op->getResult(0);
      Type t = v.getType();
      b.setInsertionPointAfter(op);
      std::string tag = "t" + std::to_string(n++);
      if (auto mt = dyn_cast<MemType>(t)) {
        auto i8 = b.getIntegerType(8);
        if (!strong) {
          PrintMemOp::create(b, op->getLoc(), ValueRange{},
                             b.getStringAttr(tag), b.getStringAttr(name(v)),
                             b.getStringAttr("a" + tag), TypeAttr::get(i8),
                             b.getStringAttr("hexdump"), b.getI64IntegerAttr(4),
                             StringAttr(), IntegerAttr());
          continue;
        }
        // an address of the index type defined before, in this block
        Value addr;
        for (Operation *p = op->getPrevNode(); p && !addr; p = p->getPrevNode())
          for (Value r : p->getResults())
            if (r.getType() == mt.getIndex())
              addr = r;
        if (addr)
          PrintMemOp::create(b, op->getLoc(), ValueRange{v, addr},
                             b.getStringAttr(tag), StringAttr(), StringAttr(),
                             TypeAttr::get(i8), b.getStringAttr("hexdump"),
                             b.getI64IntegerAttr(4), StringAttr(),
                             IntegerAttr());
        continue;
      }
      if (!isa<IntegerType, AddrType, PtrType, StructType>(t))
        continue;
      if (strong) {
        if (all || !v.use_empty())
          PrintOp::create(b, op->getLoc(), ValueRange{v}, b.getStringAttr(tag),
                          StringAttr(), TypeAttr(), StringAttr(), ArrayAttr());
      } else {
        PrintOp::create(b, op->getLoc(), ValueRange{}, b.getStringAttr(tag),
                        b.getStringAttr(name(v)), TypeAttr::get(t),
                        StringAttr(), ArrayAttr());
      }
    }
    b.setInsertionPoint(block.getTerminator());
    if (isa<HaltOp>(block.getTerminator()))
      PrintOp::create(b, loc, ValueRange{}, b.getStringAttr("halt"),
                      StringAttr(), TypeAttr(), StringAttr(), ArrayAttr());
    PrintEndOp::create(b, loc, b.getStringAttr("bb"));
  }
}

} // namespace

std::unique_ptr<Pass> createTestInsertPrintsPass() {
  return std::make_unique<TestInsertPrintsPass>();
}

} // namespace sea
