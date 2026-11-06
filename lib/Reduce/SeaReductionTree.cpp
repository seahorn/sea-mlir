// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaReductionTree.cpp - pattern-only reduction tree --------*- C++
//-*-===//
//
// sea-reduction-tree{test=<script> test-arg=<a>,... pattern=<p>,...}: the
// search of upstream's reduction-tree (single path over ranges of ops,
// mlir/lib/Reducer/ReductionTreePass.cpp: findOptimal; built on its
// ReductionNode and Tester) without its op-deletion phases: only the sea
// patterns are applied, to the ops outside the kept ranges of each
// program's region. Why: upstream first deletes ranges of ops, which in a
// large CFG mostly gives candidates the verifier rejects and, for the few
// that verify, one test per handful of ops (the h2fc export: 14k ops,
// hours); the patterns shrink a program by halves. Also: folding is off
// (only the patterns change the program), and a variant on which no
// pattern matched is not tested (it equals its parent).
//
// Contract. Accepts: a module of sea.programs, interesting to the test.
// Produces: an interesting module, no larger (printed size).
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Reduce/Reduce.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Reducer/ReductionNode.h"
#include "mlir/Reducer/Tester.h"
#include "mlir/Rewrite/FrozenRewritePatternSet.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/RegionUtils.h"
#include "llvm/Support/Allocator.h"

using namespace mlir;

namespace sea {
namespace {

/// Applies the patterns to the ops of \p region outside \p keep (sorted
/// ranges of op indices). True if anything changed.
static bool applyOutside(Region &region, const FrozenRewritePatternSet &pats,
                         ArrayRef<ReductionNode::Range> keep,
                         bool eraseUnreachable) {
  std::vector<Operation *> ops;
  size_t k = 0;
  for (auto [index, op] : llvm::enumerate(region.getOps())) {
    int i = static_cast<int>(index);
    if (k < keep.size() && i == keep[k].second)
      ++k;
    if (k == keep.size() || i < keep[k].first)
      ops.push_back(&op);
  }
  GreedyRewriteConfig config;
  config.setStrictness(GreedyRewriteStrictness::ExistingOps)
      .enableFolding(false);
  bool any = false;
  for (Operation *op : ops) {
    bool changed = false;
    (void)applyOpPatternsGreedily(ArrayRef<Operation *>{op}, pats, config,
                                  &changed);
    any |= changed;
  }
  // Blocks a select-branch disconnected: sea-vc still encodes them, so a
  // symptom can hide there; erased, the candidate keeps it only in code
  // that runs.
  if (any && eraseUnreachable) {
    IRRewriter rewriter(region.getContext());
    (void)eraseUnreachableBlocks(rewriter, region);
  }
  return any;
}

class SeaReductionTreePass
    : public PassWrapper<SeaReductionTreePass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SeaReductionTreePass)
  SeaReductionTreePass() = default;
  SeaReductionTreePass(const SeaReductionTreePass &o) : PassWrapper(o) {}

  StringRef getArgument() const final { return "sea-reduction-tree"; }
  StringRef getDescription() const final {
    return "reduction-tree's single-path search with the sea patterns only "
           "(no op-deletion phase)";
  }

  Option<std::string> test{*this, "test",
                           llvm::cl::desc("The interestingness test")};
  ListOption<std::string> testArgs{*this, "test-arg",
                                   llvm::cl::desc("Its arguments")};
  ListOption<std::string> pattern{*this, "pattern",
                                  llvm::cl::desc("Patterns (default: all)")};
  Option<bool> eraseUnreachable{
      *this, "erase-unreachable",
      llvm::cl::desc("Erase the blocks a variant left unreachable (default "
                     "on: a symptom then stays in code that runs; sea-vc "
                     "encodes unreachable blocks too)"),
      llvm::cl::init(true)};
  Option<unsigned> maxTests{
      *this, "max-tests",
      llvm::cl::desc("Stop the search after this many tests per program, "
                     "keeping the smallest interesting variant (0: no "
                     "limit). The single-path search ends in single-op "
                     "variants, one test each: a long tail on large "
                     "programs"),
      llvm::cl::init(0)};

  LogicalResult initialize(MLIRContext *ctx) override {
    RewritePatternSet set(ctx);
    if (!populateSeaReductionPatterns(set, pattern))
      return emitError(UnknownLoc::get(ctx))
             << "sea-reduction-tree: unknown pattern";
    patterns = FrozenRewritePatternSet(std::move(set));
    return success();
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    Tester tester(test, testArgs);
    SmallVector<ProgramOp> programs(module.getOps<ProgramOp>());
    for (ProgramOp p : programs)
      if (failed(reduce(module, p.getBody(), tester)))
        return signalPassFailure();
  }

private:
  LogicalResult reduce(ModuleOp module, Region &region, const Tester &test) {
    auto init = test.isInteresting(module);
    if (init.first != Tester::Interestingness::True)
      return module.emitError() << "uninterested module will not be reduced";
    llvm::SpecificBumpPtrAllocator<ReductionNode> allocator;
    std::vector<ReductionNode::Range> ranges{
        {0, std::distance(region.op_begin(), region.op_end())}};
    ReductionNode *root = allocator.Allocate();
    new (root) ReductionNode(nullptr, ranges, allocator);
    if (failed(root->initialize(module, region)))
      return failure();
    ReductionNode *smallest = root;
    ReductionNode::iterator<TraversalMode::SinglePath> iter(root);
    unsigned tests = 0;
    while (iter != ReductionNode::iterator<TraversalMode::SinglePath>::end()) {
      if (maxTests != 0 && tests >= maxTests) {
        llvm::errs() << "sea-reduction-tree.max-tests: reached\n";
        break;
      }
      ReductionNode &node = *iter;
      bool isRoot = node.getParent() == &node; // upstream: the root's parent
      if (isRoot) {
        node.update(init);
      } else if (applyOutside(node.getRegion(), patterns, node.getRanges(),
                              eraseUnreachable)) {
        node.update(test.isInteresting(node.getModule()));
        ++tests;
      } else {
        // Equal to its parent: nothing to learn (and not smaller).
        node.update({Tester::Interestingness::False, 0});
      }
      if (node.isInteresting() == Tester::Interestingness::True &&
          node.getSize() < smallest->getSize())
        smallest = &node;
      ++iter;
    }
    // Replay the path to the smallest variant on the module itself.
    SmallVector<ReductionNode *> trace;
    for (ReductionNode *n = smallest;; n = n->getParent()) {
      trace.push_back(n);
      if (n->getParent() == n)
        break;
    }
    while (!trace.empty())
      (void)applyOutside(region, patterns,
                         trace.pop_back_val()->getStartRanges(),
                         eraseUnreachable);
    auto final = test.isInteresting(module);
    if (final.first != Tester::Interestingness::True)
      return module.emitError()
             << "sea-reduction-tree: the replayed reduction is not "
                "interesting (a pattern is not deterministic?)";
    return success();
  }

  FrozenRewritePatternSet patterns;
};

} // namespace

void registerSeaReductionTreePass() {
  registerPass([] { return std::make_unique<SeaReductionTreePass>(); });
}

} // namespace sea
