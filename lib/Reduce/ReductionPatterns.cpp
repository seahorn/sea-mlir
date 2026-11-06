// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ReductionPatterns.cpp - sea reduction patterns and passes -*- C++
//-*-===//
//
// The rewrites sea-reduce uses to shrink a program (include/sea/Reduce/
// Reduce.h lists them). Contract of every pattern: the result verifies
// whenever the input did (types match, nondet sites stay unique, the entry
// block keeps no arguments, terminators stay terminators), and it erases
// only the op it matched. Meaning is not kept: that is the interestingness
// test's business.
//
// Passes.
//   sea-reduce-apply{pattern=<name>,... limit=<n> op-index=<i> report}:
//     the patterns to the first n ops they match (0: all), in program
//     order, or to op i only; report prints the indices rewritten (what
//     utils/reduce-test.py audit samples). Accepts: any
//     verified module. Produces: a verified module.
//   sea-reduce-cleanup: dead ops, unreachable blocks, unused block
//     arguments, single-predecessor br chains. Accepts / produces: the same.
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Reduce/Reduce.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Rewrite/FrozenRewritePatternSet.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/RegionUtils.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;

namespace sea {
namespace {

/// One past the largest nondet site of the program holding \p op (every op
/// with an integer `site` attribute: nondet, struct.nondet, mem.nondet,
/// memhavoc, alloca, malloc, ptr.nondet). Deterministic, so reduction-tree
/// can replay a reduction path.
static uint64_t freshSite(Operation *op) {
  auto program = op->getParentOfType<ProgramOp>();
  uint64_t next = 0;
  program.walk([&](Operation *o) {
    if (auto s = o->getAttrOfType<IntegerAttr>("site"))
      next = std::max<uint64_t>(next, s.getValue().getZExtValue() + 1);
  });
  return next;
}

static bool isMemUpdate(Operation *op) {
  return isa<StoreOp, MemSetOp, MemMoveOp, MemHavocOp, ResetModifiedOp>(op);
}

/// memory-bypass: an update's result -> its incoming memory.
struct MemoryBypass : RewritePattern {
  MemoryBypass(MLIRContext *ctx)
      : RewritePattern(MatchAnyOpTypeTag(), /*benefit=*/7, ctx) {
    setDebugName("memory-bypass");
  }
  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    if (!isMemUpdate(op))
      return failure();
    rewriter.replaceOp(op, op->getOperand(0));
    return success();
  }
};

/// drop-assume: sea.assume erased.
struct DropAssume : OpRewritePattern<AssumeOp> {
  DropAssume(MLIRContext *ctx) : OpRewritePattern(ctx, /*benefit=*/6) {
    setDebugName("drop-assume");
  }
  LogicalResult matchAndRewrite(AssumeOp op,
                                PatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};

/// select-branch: cond_br -> br. The successor kept: the one whose block
/// does not end in sea.unreachable, else the true one. The dropped block
/// may become unreachable; sea-reduce-cleanup erases it (erasing it here
/// would break reduction-tree's precollected op list).
struct SelectBranch : OpRewritePattern<CondBrOp> {
  SelectBranch(MLIRContext *ctx) : OpRewritePattern(ctx, /*benefit=*/5) {
    setDebugName("select-branch");
  }
  LogicalResult matchAndRewrite(CondBrOp op,
                                PatternRewriter &rewriter) const override {
    bool takeFalse = isa<UnreachableOp>(op.getTrueDest()->getTerminator()) &&
                     !isa<UnreachableOp>(op.getFalseDest()->getTerminator());
    Block *dest = takeFalse ? op.getFalseDest() : op.getTrueDest();
    SmallVector<Value> args(takeFalse ? op.getFalseDestOperands()
                                      : op.getTrueDestOperands());
    rewriter.replaceOpWithNewOp<BrOp>(op, ValueRange(args), dest);
    return success();
  }
};

/// load-to-nondet: a load -> a fresh nondet of its type (integer:
/// sea.nondet; pointer: sea.ptr.nondet). Addresses are left alone.
struct LoadToNondet : OpRewritePattern<LoadOp> {
  LoadToNondet(MLIRContext *ctx) : OpRewritePattern(ctx, /*benefit=*/4) {
    setDebugName("load-to-nondet");
  }
  LogicalResult matchAndRewrite(LoadOp op,
                                PatternRewriter &rewriter) const override {
    Type t = op.getResult().getType();
    if (!isa<IntegerType, PtrType>(t))
      return failure();
    uint64_t site = freshSite(op);
    if (isa<IntegerType>(t))
      rewriter.replaceOpWithNewOp<NondetOp>(op, t, kReduceNondetCallee, site);
    else
      rewriter.replaceOpWithNewOp<PtrNondetOp>(op, t, kReduceNondetCallee,
                                               site);
    return success();
  }
};

/// A pure value op of the sea dialect with one non-memory result: what
/// forward-operand and zero-const may replace.
static bool isValueOp(Operation *op) {
  if (!isa<SeaDialect>(op->getDialect()) || op->getNumResults() != 1 ||
      op->getNumRegions() != 0 || op->hasTrait<OpTrait::IsTerminator>() ||
      op->hasTrait<OpTrait::ConstantLike>())
    return false;
  if (isa<MemType>(op->getResult(0).getType()))
    return false;
  // Ops with a site are inputs, not computations; a load has its own
  // pattern first (forward-operand still applies to a pointer load from a
  // pointer of the same type, where load-to-nondet does not).
  return !op->hasAttr("site");
}

/// forward-operand: the result -> the first operand of the same type.
struct ForwardOperand : RewritePattern {
  ForwardOperand(MLIRContext *ctx)
      : RewritePattern(MatchAnyOpTypeTag(), /*benefit=*/3, ctx) {
    setDebugName("forward-operand");
  }
  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    if (!isValueOp(op))
      return failure();
    Type t = op->getResult(0).getType();
    for (Value v : op->getOperands())
      if (v.getType() == t) {
        rewriter.replaceOp(op, v);
        return success();
      }
    return failure();
  }
};

/// zero-const: an integer result with no operand of its type -> 0.
struct ZeroConst : RewritePattern {
  ZeroConst(MLIRContext *ctx)
      : RewritePattern(MatchAnyOpTypeTag(), /*benefit=*/2, ctx) {
    setDebugName("zero-const");
  }
  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    if (!isValueOp(op))
      return failure();
    auto t = dyn_cast<IntegerType>(op->getResult(0).getType());
    if (!t || llvm::any_of(op->getOperandTypes(),
                           [&](Type o) { return o == Type(t); }))
      return failure();
    rewriter.replaceOpWithNewOp<ConstOp>(op, t, IntegerAttr::get(t, 0));
    return success();
  }
};

/// An op the reducer may erase once its results are unused: any op with
/// results, no regions, not a terminator (nondets, memory updates and
/// allocations included: nothing reads them).
static bool isDead(Operation *op) {
  return op->getNumResults() != 0 && op->getNumRegions() == 0 &&
         !op->hasTrait<OpTrait::IsTerminator>() && op->use_empty();
}

/// erase-dead.
struct EraseDead : RewritePattern {
  EraseDead(MLIRContext *ctx)
      : RewritePattern(MatchAnyOpTypeTag(), /*benefit=*/8, ctx) {
    setDebugName("erase-dead");
  }
  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    if (!isDead(op))
      return failure();
    rewriter.eraseOp(op);
    return success();
  }
};

static const llvm::StringRef kNames[] = {
    "memory-bypass",   "drop-assume", "select-branch", "load-to-nondet",
    "forward-operand", "zero-const",  "erase-dead"};

} // namespace

llvm::ArrayRef<llvm::StringRef> reductionPatternNames() { return kNames; }

bool populateSeaReductionPatterns(RewritePatternSet &patterns,
                                  llvm::ArrayRef<std::string> names) {
  MLIRContext *ctx = patterns.getContext();
  // `none`: no pattern (reduction-tree then only deletes ops).
  for (const std::string &n : names)
    if (n != "none" && !llvm::is_contained(kNames, n))
      return false;
  auto want = [&](llvm::StringRef n) {
    return names.empty() || llvm::is_contained(names, n.str());
  };
  if (want("memory-bypass"))
    patterns.add<MemoryBypass>(ctx);
  if (want("drop-assume"))
    patterns.add<DropAssume>(ctx);
  if (want("select-branch"))
    patterns.add<SelectBranch>(ctx);
  if (want("load-to-nondet"))
    patterns.add<LoadToNondet>(ctx);
  if (want("forward-operand"))
    patterns.add<ForwardOperand>(ctx);
  if (want("zero-const"))
    patterns.add<ZeroConst>(ctx);
  if (want("erase-dead"))
    patterns.add<EraseDead>(ctx);
  return true;
}

namespace {

class ReduceApplyPass
    : public PassWrapper<ReduceApplyPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ReduceApplyPass)
  ReduceApplyPass() = default;
  ReduceApplyPass(const ReduceApplyPass &o) : PassWrapper(o) {}

  StringRef getArgument() const final { return "sea-reduce-apply"; }
  StringRef getDescription() const final {
    return "Apply sea reduction patterns (memory-bypass, drop-assume, "
           "select-branch, load-to-nondet, forward-operand, zero-const, "
           "erase-dead) to the first `limit` ops they match";
  }

  ListOption<std::string> pattern{
      *this, "pattern",
      llvm::cl::desc("Patterns to apply (default: all), by name")};
  Option<unsigned> limit{
      *this, "limit", llvm::cl::desc("Rewrite at most this many ops (0: all)"),
      llvm::cl::init(0)};
  Option<bool> eraseUnreachable{
      *this, "erase-unreachable",
      llvm::cl::desc("Then erase the blocks left unreachable"),
      llvm::cl::init(false)};
  Option<int> opIndex{
      *this, "op-index",
      llvm::cl::desc("Only the op with this index in program order (-1: "
                     "all); the indices `report` prints"),
      llvm::cl::init(-1)};
  Option<bool> report{
      *this, "report",
      llvm::cl::desc("Print `sea-reduce-apply.match: index=<n> op=<name>` "
                     "(stderr) for each op rewritten"),
      llvm::cl::init(false)};

  LogicalResult initialize(MLIRContext *ctx) override {
    RewritePatternSet set(ctx);
    if (!populateSeaReductionPatterns(set, pattern)) {
      std::string all;
      for (llvm::StringRef n : kNames)
        all += (all.empty() ? "" : ", ") + n.str();
      return emitError(UnknownLoc::get(ctx))
             << "sea-reduce-apply: unknown pattern (known: " << all << ")";
    }
    patterns = FrozenRewritePatternSet(std::move(set));
    return success();
  }

  void runOnOperation() override {
    SmallVector<Operation *> ops;
    getOperation().walk([&](ProgramOp p) {
      p.getBody().walk([&](Operation *op) { ops.push_back(op); });
    });
    // Folding off: only the patterns change the program.
    GreedyRewriteConfig config;
    config.setStrictness(GreedyRewriteStrictness::ExistingOps)
        .enableFolding(false);
    // The greedy driver erases a trivially dead op whatever the patterns:
    // leave those to erase-dead, so a match is the pattern's own.
    bool eraseDead =
        pattern.empty() || llvm::is_contained(pattern, "erase-dead");
    unsigned done = 0;
    for (auto [i, op] : llvm::enumerate(ops)) {
      if (limit != 0 && done >= limit)
        break;
      if (opIndex >= 0 && static_cast<int>(i) != opIndex)
        continue;
      if (!eraseDead && isOpTriviallyDead(op))
        continue;
      std::string name = op->getName().getStringRef().str();
      bool changed = false;
      (void)applyOpPatternsGreedily(ArrayRef<Operation *>{op}, patterns, config,
                                    &changed);
      done += changed;
      if (changed && report)
        llvm::errs() << "sea-reduce-apply.match: index=" << i << " op=" << name
                     << "\n";
    }
    if (eraseUnreachable && done != 0) {
      IRRewriter rewriter(&getContext());
      getOperation().walk([&](ProgramOp p) {
        (void)eraseUnreachableBlocks(rewriter, p.getBody());
      });
    }
  }

private:
  FrozenRewritePatternSet patterns;
};

/// Merges a block into its only predecessor when that one ends in a br to
/// it: the block's arguments become the br's operands. Returns true if any.
static bool mergeIntoPredecessors(Region &region, RewriterBase &rewriter) {
  bool changed = false;
  for (Block &b : llvm::make_early_inc_range(llvm::drop_begin(region))) {
    Block *pred = b.getSinglePredecessor();
    if (!pred || pred == &b)
      continue;
    auto br = dyn_cast<BrOp>(pred->getTerminator());
    if (!br)
      continue;
    SmallVector<Value> args(br.getDestOperands());
    rewriter.eraseOp(br);
    rewriter.mergeBlocks(&b, pred, args);
    changed = true;
  }
  return changed;
}

class ReduceCleanupPass
    : public PassWrapper<ReduceCleanupPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ReduceCleanupPass)
  ReduceCleanupPass() = default;
  ReduceCleanupPass(const ReduceCleanupPass &o) : PassWrapper(o) {}

  Option<bool> deadOps{*this, "dead-ops",
                       llvm::cl::desc("Erase ops with unused results"),
                       llvm::cl::init(true)};
  Option<bool> blocks{
      *this, "blocks",
      llvm::cl::desc("Erase unreachable blocks and unused block arguments; "
                     "merge br chains"),
      llvm::cl::init(true)};

  StringRef getArgument() const final { return "sea-reduce-cleanup"; }
  StringRef getDescription() const final {
    return "Erase dead ops (nondets and memory updates too), unreachable "
           "blocks and unused block arguments; merge br chains";
  }

  void runOnOperation() override {
    IRRewriter rewriter(&getContext());
    getOperation().walk([&](ProgramOp p) {
      Region &body = p.getBody();
      bool changed = true;
      while (changed) {
        changed = false;
        // Dead ops, users first: walk blocks backwards.
        for (Block &b : body) {
          if (!deadOps)
            break;
          for (Operation &op :
               llvm::make_early_inc_range(llvm::reverse(b.getOperations())))
            if (isDead(&op)) {
              rewriter.eraseOp(&op);
              changed = true;
            }
        }
        if (!blocks)
          continue;
        // Unreachable blocks, dead block arguments (no block merging:
        // identical blocks merged could give a cond_br equal targets).
        changed |= succeeded(simplifyRegions(rewriter, body,
                                             /*mergeBlocks=*/false));
        changed |= mergeIntoPredecessors(body, rewriter);
      }
    });
  }
};

} // namespace

std::unique_ptr<Pass> createReduceApplyPass() {
  return std::make_unique<ReduceApplyPass>();
}
std::unique_ptr<Pass> createReduceCleanupPass() {
  return std::make_unique<ReduceCleanupPass>();
}

void registerReducePasses() {
  registerPass([] { return createReduceApplyPass(); });
  registerPass([] { return createReduceCleanupPass(); });
}

} // namespace sea
