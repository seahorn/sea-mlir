// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExpandSmallMemMove.cpp - short memmoves as copies --------*- C++ -*-===//
//
// Under --seam a memmove is one memory-valued term (mcopy,
// docs/design/SeaM-memops.md), read through one range test. When SeaM
// cannot decide that test (a symbolic destination, a source written at
// another width), every read of the result against the move leaves a read
// of the source at a symbolic index: Ackermann residue. A short memmove is
// cheaper as explicit copies, each a plain load and store that SeaM
// resolves cell by cell. This pass rewrites a memmove whose length is a
// constant n <= K (bytes, option max-bytes) as
//
//   v_i = load src[s + i]          for i in 0..n-1  (all loads first)
//   m_1 = store m[d + 0], v_0 ; ... ; m_n = store m_{n-1}[d + n-1], v_{n-1}
//
// Exact memmove semantics, overlap included: every source cell is read from
// the source version before the op, as the op reads it. Straight-line code:
// no new blocks, no assumptions. A length 0 memmove is its input memory.
//
// Attributes. `inplace` is kept on every store (each intermediate version
// dies at the next store; the loads come first, so a source that is the
// destination's version is read before it is updated). `align` /
// `src_align` A promise d = 0 (mod A); a copy at d + i keeps the largest
// power of 2 that divides both A and i (A at i = 0; none when that is 1).
//
// Contract.
//   Accepts: any program. Runs after the metadata lowerings and placement
//     in the VC lowering (fat-pointer companion memmoves are plain memmoves
//     of the same length then, and get the same decision).
//   Expands: sea.memmove with a constant length (a sea.const or a value
//     the folders compute from constants) n <= K, on an address-indexed
//     (placed) memory with the exact or keyed layout, without `tracked`.
//   Keeps (counted by reason): a symbolic length (`symbolic`), n > K
//     (`over-k`), a word layout (`word-layout`: the VC copies its words
//     itself), a pointer-indexed memory (`abstract`: before placement a
//     pointer's bytes have no value, so the abstract interpreter refuses a
//     byte load of a stored pointer that the memmove copies whole), a
//     `tracked` memmove (`tracked`: sea-lower-tracking has not
//     run; tracked stores per cell would mean the same, not done).
//   Reports: the counters (kExpandMemMoveStatsAttr on the program;
//     sea-vc prints vc.expand-memmove) and the lower.expand-memmove log.
//   Produces: no sea.memmove the pass expands (checked).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Log.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

log::Tag logExpandMemMove("lower.expand-memmove", 2,
                          "sea-expand-small-memmove: the counters of a run "
                          "(1), each memmove expanded or kept (2)");

class ExpandSmallMemMovePass
    : public PassWrapper<ExpandSmallMemMovePass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ExpandSmallMemMovePass)

  ExpandSmallMemMovePass() = default;
  ExpandSmallMemMovePass(const ExpandSmallMemMovePass &other)
      : PassWrapper(other) {}
  explicit ExpandSmallMemMovePass(unsigned k) { maxBytes = k; }

  StringRef getArgument() const final { return "sea-expand-small-memmove"; }
  StringRef getDescription() const final {
    return "Expand memmoves of a constant length <= max-bytes into loads of "
           "every source cell followed by stores in address order (exact, "
           "overlap included)";
  }

  void runOnOperation() override;

  Option<unsigned> maxBytes{
      *this, "max-bytes",
      llvm::cl::desc("Largest constant length (bytes) expanded; 0: none"),
      llvm::cl::init(0)};
};

/// The promise of an access at base + i when base = 0 (mod a): the largest
/// power of 2 dividing a and i; none (null) when that is 1.
static IntegerAttr alignAt(OpBuilder &b, IntegerAttr a, uint64_t i) {
  if (!a)
    return {};
  uint64_t al = a.getInt();
  if (i != 0)
    al = std::min<uint64_t>(al, i & (~i + 1));
  if (al <= 1)
    return {};
  return b.getI64IntegerAttr(al);
}

void ExpandSmallMemMovePass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  struct Counts {
    uint64_t expanded = 0, bytes = 0, symbolic = 0, overK = 0, words = 0,
             tracked = 0, abstract = 0;
  } n;
  SmallVector<std::pair<MemMoveOp, uint64_t>> todo;
  program.walk([&](MemMoveOp op) {
    StringRef kept;
    std::optional<APInt> len = foldConstant(op.getLen());
    if (!len)
      kept = "symbolic", ++n.symbolic;
    else if (len->ugt(maxBytes))
      kept = "over-k", ++n.overK;
    else if (op.getMem().getType().isWordLayout())
      kept = "word-layout", ++n.words;
    else if (isa<PtrType>(op.getMem().getType().getIndex()))
      kept = "abstract", ++n.abstract;
    else if (op.getTracked())
      kept = "tracked", ++n.tracked;
    if (!kept.empty()) {
      SEA_LOG(logExpandMemMove, log::Object)
          .msg("kept")
          .kv("reason", kept)
          .at(op.getLoc());
      return;
    }
    todo.push_back({op, len->getZExtValue()});
  });

  OpBuilder b(ctx);
  for (auto [op, len] : todo) {
    Location loc = op.getLoc();
    MemType memTy = op.getMem().getType();
    b.setInsertionPoint(op);
    SmallVector<Value> cells;
    for (uint64_t i = 0; i < len; ++i) {
      Value at = offsetIndex(b, loc, op.getSrcAddr(), i);
      cells.push_back(LoadOp::create(b, loc, memTy.getCell(), op.getSrc(), at,
                                     alignAt(b, op.getSrcAlignAttr(), i)));
    }
    Value mem = op.getMem();
    for (uint64_t i = 0; i < len; ++i) {
      Value at = offsetIndex(b, loc, op.getAddr(), i);
      mem = StoreOp::create(b, loc, memTy, mem, at, cells[i], op.getInplace(),
                            alignAt(b, op.getAlignAttr(), i));
    }
    SEA_LOG(logExpandMemMove, log::Object)
        .msg("expanded")
        .kv("bytes", len)
        .at(loc);
    ++n.expanded;
    n.bytes += len;
    op.getResult().replaceAllUsesWith(mem);
    op.erase();
  }

  auto stat = [&](StringRef key, uint64_t v) {
    return NamedAttribute(StringAttr::get(ctx, key),
                          IntegerAttr::get(IntegerType::get(ctx, 64), v));
  };
  SmallVector<NamedAttribute> stats{stat("k", maxBytes),
                                    stat("expanded", n.expanded),
                                    stat("bytes", n.bytes),
                                    stat("kept-symbolic", n.symbolic),
                                    stat("kept-over-k", n.overK),
                                    stat("kept-word-layout", n.words),
                                    stat("kept-abstract", n.abstract),
                                    stat("kept-tracked", n.tracked)};
  program->setAttr(kExpandMemMoveStatsAttr, DictionaryAttr::get(ctx, stats));
  SEA_LOG(logExpandMemMove, log::Flow)
      .kv("k", static_cast<uint64_t>(maxBytes))
      .kv("expanded", n.expanded)
      .kv("bytes", n.bytes)
      .kv("kept-symbolic", n.symbolic)
      .kv("kept-over-k", n.overK)
      .kv("kept-word-layout", n.words)
      .kv("kept-abstract", n.abstract)
      .kv("kept-tracked", n.tracked);

  program.walk([&](MemMoveOp op) {
    std::optional<APInt> len = foldConstant(op.getLen());
    MemType t = op.getMem().getType();
    if (len && len->ule(maxBytes) && !t.isWordLayout() &&
        !isa<PtrType>(t.getIndex()) && !op.getTracked()) {
      (void)postconditionFailed("sea-expand-small-memmove",
                                "a memmove it expands", op);
      signalPassFailure();
    }
  });
}

} // namespace

std::unique_ptr<Pass> createExpandSmallMemMovePass(unsigned maxBytes) {
  return std::make_unique<ExpandSmallMemMovePass>(maxBytes);
}
std::unique_ptr<Pass> createExpandSmallMemMovePass() {
  return std::make_unique<ExpandSmallMemMovePass>();
}

} // namespace sea
