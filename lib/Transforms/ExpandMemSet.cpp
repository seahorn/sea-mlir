// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExpandMemSet.cpp - memset with a symbolic length ---------*- C++ -*-===//
//
// The VC's array encoding expresses a memset only as a store chain, so a
// length that is not a constant is expanded here, into a chain of K blocks
// (K = max-length):
//
//   B:      ... ; assume(n <= K) ; br ^c0(m)
//   ^ci(mi): cond_br (i < n), ^si, ^join(mi)          for i in 0..K-1
//   ^si:     br ^c{i+1}(store mi[p + i], v)
//   ^cK(mK): br ^join(mK)
//   ^join(m'): the rest of B, with m' for the memset's result
//
// Exact for n <= K; cells at or after n are never touched (no out-of-object
// accesses); no select on memories. The bound is explicit in the IR and
// reported (warning memset.symbolic-bound): SeaHorn's array encoding
// instead unrolls 16 times and silently ignores the rest
// (BvOpSem2MemRepr.cc:283-372, --horn-array-sym-memcpy-unroll-count).
//
// A bound that cuts real executions (n > K) makes the verdict unsat where it
// should be sat: hence the warning, and constant folding of lengths first.
//
// A memmove with a symbolic length is expanded the same way (as SeaHorn's
// array encoding does for memcpy), reading all K source cells first so
// overlap is harmless (see the comment at the expansion).
//
// Contract.
//   Accepts: sea.memset / sea.memmove of any length (byte layouts); constant
//     lengths are left alone (the VC encodes them); lengths of memset /
//     memmove / memhavoc computed from constants by pure ops are folded into
//     a sea.const (foldConstant).
//   Produces: no sea.memset / sea.memmove whose length is not a sea.const
//     (checked); a
//     sea.assume (mode=assume, default) or sea.assert (mode=assert: several
//     asserts, not BMC form) of n <= K per expanded memset (an assume
//     carries {sea.memset_bound = K}, kMemSetBoundAttr); new blocks
//     with critical edges (sea-split-critical-edges runs after).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

class ExpandMemSetPass
    : public PassWrapper<ExpandMemSetPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ExpandMemSetPass)

  ExpandMemSetPass() = default;
  ExpandMemSetPass(const ExpandMemSetPass &other) : PassWrapper(other) {}

  StringRef getArgument() const final { return "sea-expand-memset"; }
  StringRef getDescription() const final {
    return "Expand memsets with a symbolic length into a bounded chain of "
           "stores (with an explicit assume / assert of the bound)";
  }

  void runOnOperation() override;

  Option<unsigned> maxLength{
      *this, "max-length",
      llvm::cl::desc("Cells a symbolic memset may set (SeaHorn: 16)"),
      llvm::cl::init(16)};
  Option<std::string> mode{
      *this, "mode",
      llvm::cl::desc("assume (default) or assert that the length fits"),
      llvm::cl::init("assume")};
};

static bool isConstant(Value v) { return v.getDefiningOp<ConstOp>(); }

/// The assume of a bound (mode=assume) carries the bound K as the
/// discardable attribute sea.memset_bound: the under-approximation stays
/// visible in the program, and sea-vc reports it (assume.memset-bound).
static void markBound(AssumeOp a, uint64_t k) {
  a->setAttr(kMemSetBoundAttr,
             IntegerAttr::get(IntegerType::get(a->getContext(), 64), k));
}

/// Block ids (kBlockIdsAttr), if the program has them: `before` keeps the
/// ids of the expanded op's block, `join` is a piece of it; the blocks of
/// the expansion, all between them, are new (role m).
static void keepBlockIds(Block *before, Block *join, BlockIdNamer &ids) {
  for (auto it = std::next(Region::iterator(before));
       it != Region::iterator(join); ++it)
    BlockIdNamer::set(it->getTerminator(), ids.derive(join, "m"));
  if (ArrayAttr l = ids.splitTail(join))
    before->getTerminator()->setAttr(kBlockIdsAttr, l);
}

void ExpandMemSetPass::runOnOperation() {
  if (mode != "assume" && mode != "assert") {
    getOperation().emitError("sea-expand-memset: mode must be assume or "
                             "assert");
    return signalPassFailure();
  }
  OpBuilder b(&getContext());
  BlockIdNamer ids(getOperation());
  SmallVector<MemSetOp> todo;
  SmallVector<MemMoveOp> moves;
  // A length computed from constants (calloc's n * size) is a constant:
  // the VC encodes it as a store chain, with no bound. Done for every range
  // op (memset, memmove, memhavoc), whose lengths the VC needs constant.
  getOperation().walk([&](Operation *op) {
    if (!isa<MemSetOp, MemMoveOp, MemHavocOp>(op))
      return;
    OpOperand &len = isa<MemSetOp>(op) ? cast<MemSetOp>(op).getLenMutable()
                     : isa<MemMoveOp>(op)
                         ? cast<MemMoveOp>(op).getLenMutable()
                         : cast<MemHavocOp>(op).getLenMutable();
    if (isConstant(len.get()))
      return;
    if (std::optional<APInt> c = foldConstant(len.get())) {
      b.setInsertionPoint(op);
      Type t = len.get().getType();
      len.set(ConstOp::create(b, op->getLoc(), t, IntegerAttr::get(t, *c)));
      return;
    }
    if (auto ms = dyn_cast<MemSetOp>(op))
      todo.push_back(ms);
    else if (auto mv = dyn_cast<MemMoveOp>(op))
      moves.push_back(mv);
  });

  for (MemSetOp op : todo) {
    Location loc = op.getLoc();
    MemType memTy = op.getMem().getType();
    if (memTy.isWordLayout()) {
      diag::error("memset.word-layout",
                  "sea-expand-memset: symbolic-length memset on a word layout "
                  "is not supported yet",
                  loc);
      return signalPassFailure();
    }
    unsigned w = memTy.getIndexWidth();
    auto iW = b.getIntegerType(w);
    auto cst = [&](uint64_t v) -> Value {
      return ConstOp::create(b, loc, iW, IntegerAttr::get(iW, APInt(w, v)));
    };
    Value n = op.getLen();
    uint64_t k = maxLength;
    if (w < 64 && k > (uint64_t(1) << w) - 1)
      k = (uint64_t(1) << w) - 1; // every length fits
    diag::warn("memset.symbolic-bound",
               "sea-expand-memset: a memset with a symbolic length sets at "
               "most " +
                   Twine(k) + " cells; " + mode.getValue() +
                   "d in the IR (option max-length)",
               loc);

    // B up to the memset, then the bound.
    Block *before = op->getBlock();
    Block *join = before->splitBlock(op);
    BlockArgument result = join->addArgument(memTy, loc);
    b.setInsertionPointToEnd(before);
    Value fits = ICmpOp::create(b, loc, ICmpPredicate::ule, n, cst(k));
    if (mode == "assume")
      markBound(AssumeOp::create(b, loc, fits), k);
    else
      AssertOp::create(b, loc, fits);

    // The chain ^c0 .. ^cK, each with the current memory version.
    SmallVector<Block *> checks;
    for (uint64_t i = 0; i <= k; ++i) {
      checks.push_back(b.createBlock(join));
      checks.back()->addArgument(memTy, loc);
    }
    b.setInsertionPointToEnd(before);
    BrOp::create(b, loc, ValueRange{op.getMem()}, checks[0]);
    for (uint64_t i = 0; i < k; ++i) {
      Value mi = checks[i]->getArgument(0);
      Block *store = b.createBlock(checks[i + 1]);
      b.setInsertionPointToEnd(store);
      Value at = offsetIndex(b, loc, op.getAddr(), i);
      // a keyed cell holds copies of the unit value
      Value val = op.getValue();
      for (unsigned c = 1; c < memTy.getUnitCopies(); ++c) {
        auto t = IntegerType::get(b.getContext(),
                                  bitWidthOf(val.getType()) +
                                      bitWidthOf(op.getValue().getType()));
        val = ConcatOp::create(b, loc, t, op.getValue(), val);
      }
      Value next = StoreOp::create(b, loc, memTy, mi, at, val, op.getInplace(),
                                   IntegerAttr());
      BrOp::create(b, loc, ValueRange{next}, checks[i + 1]);
      b.setInsertionPointToEnd(checks[i]);
      Value more = ICmpOp::create(b, loc, ICmpPredicate::ult, cst(i), n);
      CondBrOp::create(b, loc, more, ValueRange{}, ValueRange{mi}, store, join);
    }
    b.setInsertionPointToEnd(checks[k]);
    BrOp::create(b, loc, ValueRange{checks[k]->getArgument(0)}, join);

    keepBlockIds(before, join, ids);
    op.getResult().replaceAllUsesWith(result);
    op.erase();
  }

  // memmove: the same bound; all K source cells are read first (overlap is
  // harmless: memmove reads its source as it was), at offset i < n ? i : 0
  // so every read is in bounds once n >= 1; n = 0 skips the copy.
  //   B:     ... ; assume(n <= K) ; cond_br (n == 0), ^join(m), ^read
  //   ^read: v_i = src[s + (i < n ? i : 0)]  (i in 0..K-1) ; br ^c0(m)
  //   ^ci(mi): cond_br (i < n), ^wi, ^join(mi)   ^wi: br ^c{i+1}(mi[d+i] :=
  //   v_i)
  for (MemMoveOp op : moves) {
    Location loc = op.getLoc();
    MemType memTy = op.getMem().getType();
    if (memTy.isWordLayout()) {
      diag::error("memset.word-layout",
                  "sea-expand-memset: symbolic-length memmove on a word "
                  "layout is not supported yet",
                  loc);
      return signalPassFailure();
    }
    unsigned w = memTy.getIndexWidth();
    auto iW = b.getIntegerType(w);
    auto cst = [&](uint64_t v) -> Value {
      return ConstOp::create(b, loc, iW, IntegerAttr::get(iW, APInt(w, v)));
    };
    Value n = op.getLen();
    uint64_t k = maxLength;
    if (w < 64 && k > (uint64_t(1) << w) - 1)
      k = (uint64_t(1) << w) - 1;
    diag::warn("memset.symbolic-bound",
               "sea-expand-memset: a memmove with a symbolic length copies at "
               "most " +
                   Twine(k) + " cells; " + mode.getValue() +
                   "d in the IR (option max-length)",
               loc);
    Block *before = op->getBlock();
    Block *join = before->splitBlock(op);
    BlockArgument result = join->addArgument(memTy, loc);
    b.setInsertionPointToEnd(before);
    Value fits = ICmpOp::create(b, loc, ICmpPredicate::ule, n, cst(k));
    if (mode == "assume")
      markBound(AssumeOp::create(b, loc, fits), k);
    else
      AssertOp::create(b, loc, fits);
    Value none = ICmpOp::create(b, loc, ICmpPredicate::eq, n, cst(0));
    Block *read = b.createBlock(join);
    SmallVector<Block *> checks;
    for (uint64_t i = 0; i <= k; ++i) {
      checks.push_back(b.createBlock(join));
      checks.back()->addArgument(memTy, loc);
    }
    b.setInsertionPointToEnd(before);
    CondBrOp::create(b, loc, none, ValueRange{op.getMem()}, ValueRange{}, join,
                     read);
    b.setInsertionPointToEnd(read);
    SmallVector<Value> cells;
    for (uint64_t i = 0; i < k; ++i) {
      Value inRange = ICmpOp::create(b, loc, ICmpPredicate::ult, cst(i), n);
      Value off = SelectOp::create(b, loc, iW, inRange, cst(i), cst(0));
      Value at = offsetIndex(b, loc, op.getSrcAddr(), off);
      cells.push_back(LoadOp::create(b, loc, memTy.getCell(), op.getSrc(), at,
                                     IntegerAttr()));
    }
    BrOp::create(b, loc, ValueRange{op.getMem()}, checks[0]);
    for (uint64_t i = 0; i < k; ++i) {
      Value mi = checks[i]->getArgument(0);
      Block *write = b.createBlock(checks[i + 1]);
      b.setInsertionPointToEnd(write);
      Value at = offsetIndex(b, loc, op.getAddr(), i);
      Value next = StoreOp::create(b, loc, memTy, mi, at, cells[i],
                                   op.getInplace(), IntegerAttr());
      BrOp::create(b, loc, ValueRange{next}, checks[i + 1]);
      b.setInsertionPointToEnd(checks[i]);
      Value more = ICmpOp::create(b, loc, ICmpPredicate::ult, cst(i), n);
      CondBrOp::create(b, loc, more, ValueRange{}, ValueRange{mi}, write, join);
    }
    b.setInsertionPointToEnd(checks[k]);
    BrOp::create(b, loc, ValueRange{checks[k]->getArgument(0)}, join);
    keepBlockIds(before, join, ids);
    op.getResult().replaceAllUsesWith(result);
    op.erase();
  }

  getOperation().walk([&](MemMoveOp op) {
    if (!isConstant(op.getLen())) {
      (void)postconditionFailed("sea-expand-memset",
                                "a memmove with a symbolic length", op);
      signalPassFailure();
    }
  });
  getOperation().walk([&](MemSetOp op) {
    if (!isConstant(op.getLen())) {
      (void)postconditionFailed("sea-expand-memset",
                                "a memset with a symbolic length", op);
      signalPassFailure();
    }
  });
}

} // namespace

std::unique_ptr<Pass> createExpandMemSetPass() {
  return std::make_unique<ExpandMemSetPass>();
}

} // namespace sea
