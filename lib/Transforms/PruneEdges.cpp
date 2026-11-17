// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- PruneEdges.cpp - remove edges on which no check is reached ---------===//
//
// sea-prune-edges (docs/design/PruneEdges.md). An edge of the CFG is dead
// when no execution that takes it reaches a sea.assert or a sea.halt:
//   - by its arguments: the target block has a sea.assume before its first
//     assert / halt whose condition, evaluated with the constant block
//     arguments of the edge (the op folders, so evalPureOp, over the pure
//     ops of the target block that compute it), is 0. SeaHorn's
//     `^exit(%flag): sea.assume %flag` entered with `false` is the common
//     case;
//   - by its target: every path from the target block ends blocked (a
//     sea.unreachable, an assume of constant 0, a dead edge) before an
//     assert or a halt; a block that is only `sea.unreachable` is one.
// The analysis follows chains: a block whose every edge is dead is itself
// a dead target.
// A cond_br with one dead edge becomes `sea.assume` of the condition of
// the other edge and a sea.br to it; a block with no live edge left ends
// in sea.unreachable instead of its branch. Between rounds, sea-assume-fold's
// rewrite (the new assumes fix the condition for the code after them),
// canonicalize (constant branches, dead blocks, block merges that make
// block arguments constant) and cse run; the pass stops at the first
// round with no dead edge (a fixpoint: each round removes at least one
// edge and the clean-ups add none; checked).
//
// Exact for the safety query. Only executions that reach an assert or a
// halt are observed (an assert fails or holds; reaching a halt is a
// successful run); an execution on a dead edge reaches neither: it blocks
// at the assume, or later on every path. The program after the pass has
// the same executions that reach an assert or a halt, with the same values
// and the same failures; blocked prefixes are gone. No premise, no ledger
// entry. Not for reachability of sites (sea-vc --reach): a blocked prefix
// reaches the sites before the block; sea-vc leaves the pass out there.
//
// Counterexample attribution: the branches sea-merge-asserts made of an
// assert (location `sea.assert`, MergeAsserts.cpp) keep their meaning. A
// merged cond_br whose continuation edge is dead becomes `assume !P; br
// ^land` with the marker and the assert id (every execution that goes on
// fails that assert); one whose landing edge were dead (never: the landing
// block reaches the exit's assert) would lose the marker as an assert that
// held (as SimplifyCondBr, SeaOps.cpp).
//
// Contract.
//   Accepts: any sea.program (single-region CFG). One with a cycle among
//     its reachable blocks is left alone: not BMC form, and sea-vc refuses
//     it (a removed loop that never reaches a check would hide that).
//   Produces: no block reachable from the entry has a dead edge (checked:
//     the analysis runs again after the last rewrite and finds none). The
//     new ops: the assume at the cond_br's location, the negation (sea.xor
//     with 1) and its constant without one; the new br / unreachable keep
//     the branch's location and discardable attributes (block ids).
//   Reports: the counters (kPruneEdgesStatsAttr on the program, only when
//     an edge was pruned; the lower.prune-edges log): edges (dead edges
//     removed, at the first dead edge of each path from the entry) and of
//     them by-args / dead-target; falsified (edges whose constant
//     arguments falsify the target's assume, among the blocks reachable
//     before a round: the sources of the dead edges); cond-br (cond_br ops made
//     an assume and a br), unreachable (branches replaced by
//     sea.unreachable), blocks (blocks reachable before a round and not
//     after its rewrite), rounds (rounds that pruned), fold-uses (uses
//     sea-assume-fold's rewrite folded between rounds).
//
//===----------------------------------------------------------------------===//

#include "sea/Transforms/PruneEdges.h"
#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Log.h"
#include "sea/Transforms/AssumeFold.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

using namespace mlir;

namespace sea {

static log::Tag logPrune("lower.prune-edges", 2,
                         "sea-prune-edges: the counters of a run (1), each "
                         "pruned edge (2)");

namespace {

enum class Dead { No, ByConst, ByArgs, Target };

struct Counters {
  uint64_t edges = 0, byArgs = 0, deadTarget = 0, falsified = 0, condBr = 0,
           unreachable = 0, blocks = 0, rounds = 0, foldUses = 0;
};

/// How a block's own ops end an execution before its terminator.
struct BlockScan {
  bool observes = false; // an assert / halt (or a region op) comes first
  bool blocked = false;  // an assume of constant 0 or sea.unreachable first
  SmallVector<AssumeOp> assumes; // before the first assert / halt
};

BlockScan scanBlock(Block *b) {
  BlockScan s;
  for (Operation &op : *b) {
    if (isPrintOp(&op))
      continue;
    // a region op may hold checks: an observation, conservatively
    if (isa<AssertOp, HaltOp>(op) || op.getNumRegions()) {
      s.observes = true;
      return s;
    }
    if (isa<UnreachableOp>(op)) {
      s.blocked = true;
      return s;
    }
    if (auto as = dyn_cast<AssumeOp>(op)) {
      if (matchPattern(as.getCond(), m_Zero())) {
        s.blocked = true;
        return s;
      }
      s.assumes.push_back(as);
    }
  }
  return s;
}

/// The analysis of one round.
class Analysis {
public:
  explicit Analysis(Region &r) : region(r) {
    for (Block &b : r)
      scans.try_emplace(&b, scanBlock(&b));
    computeObserving();
  }

  /// Why edge `idx` of `b`'s terminator is dead (No: it is live).
  Dead edge(Block *b, unsigned idx) {
    Operation *t = b->getTerminator();
    if (auto cbr = dyn_cast<CondBrOp>(t)) {
      APInt k;
      if (matchPattern(cbr.getCondition(), m_ConstantInt(&k)) &&
          k.isOne() != (idx == 0))
        return Dead::ByConst;
    }
    if (falsifiedByArgs(b, idx))
      return Dead::ByArgs;
    if (!observing.contains(t->getSuccessor(idx)))
      return Dead::Target;
    return Dead::No;
  }

  /// Blocks reachable from the entry over live edges.
  llvm::DenseSet<Block *> reached() {
    llvm::DenseSet<Block *> seen;
    SmallVector<Block *> todo{&region.front()};
    while (!todo.empty()) {
      Block *b = todo.pop_back_val();
      if (!seen.insert(b).second || !hasBranch(b))
        continue;
      for (unsigned i = 0, e = b->getTerminator()->getNumSuccessors(); i < e;
           ++i)
        if (edge(b, i) == Dead::No)
          todo.push_back(b->getTerminator()->getSuccessor(i));
    }
    return seen;
  }

  static bool hasBranch(Block *b) {
    return isa<BrOp, CondBrOp>(b->getTerminator());
  }

private:
  Region &region;
  llvm::DenseMap<Block *, BlockScan> scans;
  llvm::DenseSet<Block *> observing;
  llvm::DenseMap<std::pair<Block *, unsigned>, bool> argsMemo;

  /// The value of `v` with the target's arguments `env`; null if unknown.
  /// Constants, the arguments, and pure ops of `target` whose operands are
  /// known, through their folders (evalPureOp).
  Attribute eval(Value v, Block *target,
                 const llvm::DenseMap<Value, Attribute> &env,
                 llvm::DenseMap<Value, Attribute> &memo, unsigned depth) {
    if (auto it = env.find(v); it != env.end())
      return it->second;
    Attribute c;
    if (matchPattern(v, m_Constant(&c)))
      return c;
    Operation *def = v.getDefiningOp();
    if (!def || def->getBlock() != target || depth > 32 ||
        def->getNumResults() != 1 || def->getNumRegions() || !isPure(def))
      return {};
    if (auto it = memo.find(v); it != memo.end())
      return it->second;
    SmallVector<Attribute> operands;
    for (Value o : def->getOperands())
      operands.push_back(eval(o, target, env, memo, depth + 1));
    Attribute r;
    SmallVector<OpFoldResult> folded;
    if (succeeded(def->fold(operands, folded)) && folded.size() == 1) {
      if (auto a = dyn_cast<Attribute>(folded[0]))
        r = a;
      else if (auto fv = dyn_cast<Value>(folded[0]); fv && fv != v)
        r = eval(fv, target, env, memo, depth + 1); // an arm of a select
    }
    memo[v] = r;
    return r;
  }

  /// A leading assume of the target is 0 with the edge's constants.
  bool falsifiedByArgs(Block *b, unsigned idx) {
    auto key = std::make_pair(b, idx);
    if (auto it = argsMemo.find(key); it != argsMemo.end())
      return it->second;
    bool dead = false;
    auto br = cast<BranchOpInterface>(b->getTerminator());
    Block *target = br->getSuccessor(idx);
    const BlockScan &s = scans.find(target)->second;
    if (!s.assumes.empty()) {
      llvm::DenseMap<Value, Attribute> env;
      OperandRange args = br.getSuccessorOperands(idx).getForwardedOperands();
      for (auto [arg, v] : llvm::zip(target->getArguments(), args)) {
        Attribute c;
        if (matchPattern(v, m_Constant(&c)) && isa<IntegerAttr>(c))
          env[arg] = c;
      }
      if (!env.empty()) {
        llvm::DenseMap<Value, Attribute> memo;
        for (AssumeOp as : s.assumes) {
          auto k = dyn_cast_if_present<IntegerAttr>(
              eval(as.getCond(), target, env, memo, 0));
          if (k && k.getValue().isZero()) {
            dead = true;
            break;
          }
        }
      }
    }
    argsMemo[key] = dead;
    return dead;
  }

  /// observing = the blocks from which some execution reaches an assert
  /// or a halt (least fixpoint, backwards over edges not dead by their
  /// condition or arguments).
  void computeObserving() {
    SmallVector<Block *> todo;
    for (auto &[b, s] : scans)
      if (s.observes || (!s.blocked && !isa<BrOp, CondBrOp, UnreachableOp>(
                                           b->getTerminator()))) {
        // an unknown terminator: observing, conservatively
        observing.insert(b);
        todo.push_back(b);
      }
    while (!todo.empty()) {
      Block *s = todo.pop_back_val();
      for (auto it = s->pred_begin(), e = s->pred_end(); it != e; ++it) {
        Block *p = *it;
        if (observing.contains(p) || scans.find(p)->second.blocked ||
            !hasBranch(p))
          continue;
        unsigned idx = it.getSuccessorIndex();
        Operation *t = p->getTerminator();
        if (auto cbr = dyn_cast<CondBrOp>(t)) {
          APInt k;
          if (matchPattern(cbr.getCondition(), m_ConstantInt(&k)) &&
              k.isOne() != (idx == 0))
            continue;
        }
        if (falsifiedByArgs(p, idx))
          continue;
        observing.insert(p);
        todo.push_back(p);
      }
    }
  }
};

/// The discardable attributes of a merged assert's branch for a branch
/// that is no failure site: without the assert id.
DictionaryAttr marksWithout(Operation *op, StringRef drop) {
  NamedAttrList marks(op->getDiscardableAttrDictionary());
  if (!drop.empty())
    marks.erase(drop);
  return marks.getDictionary(op->getContext());
}

bool isMergedAssertBranch(Operation *op) {
  auto n = dyn_cast<NameLoc>(op->getLoc());
  return n && n.getName().getValue() == kMergedAssertLocName;
}

class PruneEdgesPass
    : public PassWrapper<PruneEdgesPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PruneEdgesPass)

  PruneEdgesPass() = default;
  PruneEdgesPass(const PruneEdgesPass &other) : PassWrapper(other) {}
  PruneEdgesPass(bool f, bool c, bool s) {
    fold = f;
    canonicalize = c;
    cse = s;
  }

  StringRef getArgument() const final { return "sea-prune-edges"; }
  StringRef getDescription() const final {
    return "Remove CFG edges on which no execution reaches an assert or a "
           "halt (constant block arguments that falsify the target's "
           "assume; targets that only block), to a fixpoint with "
           "assume-fold, canonicalize and cse";
  }

  void runOnOperation() override;

  Option<bool> fold{*this, "fold",
                    llvm::cl::desc("sea-assume-fold's rewrite between rounds"),
                    llvm::cl::init(true)};
  Option<bool> canonicalize{*this, "canonicalize",
                            llvm::cl::desc("canonicalize between rounds"),
                            llvm::cl::init(true)};
  Option<bool> cse{*this, "cse", llvm::cl::desc("cse between rounds"),
                   llvm::cl::init(true)};

private:
  /// One round: the rewrite of every dead edge of a reached block; the
  /// number of edges pruned.
  uint64_t round(ProgramOp program, Counters &n);
};

uint64_t PruneEdgesPass::round(ProgramOp program, Counters &n) {
  Region &region = program->getRegion(0);
  Analysis a(region);
  const llvm::DenseSet<Block *> reached = a.reached();
  struct Edit {
    Block *block;
    SmallVector<Dead, 2> dead;
  };
  SmallVector<Edit> edits;
  for (Block &b : region) {
    if (!reached.contains(&b) || !Analysis::hasBranch(&b))
      continue;
    Edit e{&b, {}};
    bool any = false;
    for (unsigned i = 0, k = b.getTerminator()->getNumSuccessors(); i < k;
         ++i) {
      e.dead.push_back(a.edge(&b, i));
      any |= e.dead.back() == Dead::ByArgs || e.dead.back() == Dead::Target;
    }
    bool allDead = llvm::all_of(e.dead, [](Dead d) { return d != Dead::No; });
    // a constant cond_br whose dead edge is only the one it never takes is
    // canonicalize's
    if (any && (allDead || !llvm::is_contained(e.dead, Dead::ByConst)))
      edits.push_back(std::move(e));
  }
  if (edits.empty())
    return 0;

  // blocks the rewrite cuts off: reachable before (every edge but those
  // a constant condition never takes), not reached
  llvm::DenseSet<Block *> before;
  {
    SmallVector<Block *> todo{&region.front()};
    while (!todo.empty()) {
      Block *b = todo.pop_back_val();
      if (!before.insert(b).second)
        continue;
      if (!Analysis::hasBranch(b))
        continue;
      for (unsigned i = 0, k = b->getTerminator()->getNumSuccessors(); i < k;
           ++i) {
        Dead d = a.edge(b, i);
        n.falsified += d == Dead::ByArgs;
        if (d != Dead::ByConst)
          todo.push_back(b->getTerminator()->getSuccessor(i));
      }
    }
  }
  for (Block *b : before)
    n.blocks += !reached.contains(b);

  MLIRContext *ctx = &getContext();
  OpBuilder ob(ctx);
  uint64_t pruned = 0;
  for (Edit &e : edits) {
    Operation *t = e.block->getTerminator();
    for (auto [i, d] : llvm::enumerate(e.dead)) {
      if (d != Dead::ByArgs && d != Dead::Target)
        continue;
      ++pruned;
      ++(d == Dead::ByArgs ? n.byArgs : n.deadTarget);
      SEA_LOG(logPrune, log::Object)
          .msg("edge")
          .kv("by", d == Dead::ByArgs ? "args" : "dead-target")
          .kv("succ", uint64_t(i))
          .at(t->getLoc());
    }
    ob.setInsertionPoint(t);
    const bool merged = isMergedAssertBranch(t);
    if (llvm::all_of(e.dead, [](Dead d) { return d != Dead::No; })) {
      // no live edge: the block ends here (a merged assert's branch would
      // be no failure site any more)
      auto u = UnreachableOp::create(ob, merged ? Location(ob.getUnknownLoc())
                                                : t->getLoc());
      u->setDiscardableAttrs(
          marksWithout(t, merged ? StringRef(kAssertIdAttr) : StringRef()));
      t->erase();
      ++n.unreachable;
      continue;
    }
    // a cond_br with one dead edge: assume the other edge's condition
    auto cbr = cast<CondBrOp>(t);
    const bool keepTrue = e.dead[0] == Dead::No;
    Value c = cbr.getCondition();
    if (!keepTrue) {
      auto i1 = IntegerType::get(ctx, 1);
      Value one = ConstOp::create(ob, ob.getUnknownLoc(), i1,
                                  IntegerAttr::get(i1, APInt(1, 1)));
      c = XorOp::create(ob, ob.getUnknownLoc(), c, one);
    }
    AssumeOp::create(ob, t->getLoc(), c);
    // a merged assert's branch kept on its true side: the assert held, the
    // br is no failure site (no marker, no assert id); on its false side
    // the br is the failure (marker and id kept)
    const bool held = merged && keepTrue;
    auto br = BrOp::create(
        ob, held ? Location(ob.getUnknownLoc()) : t->getLoc(),
        keepTrue ? cbr.getTrueDestOperands() : cbr.getFalseDestOperands(),
        keepTrue ? cbr.getTrueDest() : cbr.getFalseDest());
    br->setDiscardableAttrs(
        marksWithout(t, held ? StringRef(kAssertIdAttr) : StringRef()));
    t->erase();
    ++n.condBr;
  }
  n.edges += pruned;
  return pruned;
}

/// A cycle among the blocks reachable from the entry (iterative DFS).
static bool hasCycle(Region &r) {
  llvm::DenseMap<Block *, int> state; // 1: on the stack, 2: done
  SmallVector<std::pair<Block *, unsigned>> stack{{&r.front(), 0}};
  state[&r.front()] = 1;
  while (!stack.empty()) {
    auto &[b, i] = stack.back();
    Operation *t = b->getTerminator();
    if (t && i < t->getNumSuccessors()) {
      Block *s = t->getSuccessor(i++);
      int &st = state[s];
      if (st == 1)
        return true;
      if (st == 0) {
        st = 1;
        stack.push_back({s, 0});
      }
      continue;
    }
    state[b] = 2;
    stack.pop_back();
  }
  return false;
}

static uint64_t countEdges(Region &r) {
  uint64_t k = 0;
  for (Block &b : r)
    k += b.getTerminator() ? b.getTerminator()->getNumSuccessors() : 0;
  return k;
}

void PruneEdgesPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  Region &region = program->getRegion(0);
  if (region.empty())
    return markAllAnalysesPreserved();
  // a loop is not BMC form: left for sea-vc to refuse (removing one that
  // never reaches a check would hide the unsupported input)
  if (hasCycle(region))
    return markAllAnalysesPreserved();
  Counters n;
  const bool cleanup = fold || canonicalize || cse;
  OpPassManager clean(ProgramOp::getOperationName());
  if (canonicalize)
    clean.addPass(createCanonicalizerPass());
  if (cse)
    clean.addPass(createCSEPass());
  // Terminates: each round removes at least one edge, and the clean-ups
  // add none (sea-assume-fold changes operands; canonicalize folds
  // branches, merges and erases blocks; an assume of 0 splits its block
  // into one ending in sea.unreachable and a dead rest). Checked.
  while (true) {
    const uint64_t edgesBefore = countEdges(region);
    if (!round(program, n))
      break;
    ++n.rounds;
    if (!cleanup) {
      // one round: its analysis already followed every chain; the check
      // below is the postcondition
      if (round(program, n)) {
        (void)postconditionFailed("sea-prune-edges",
                                  "a dead edge after its one round", program);
        return signalPassFailure();
      }
      break;
    }
    if (fold) {
      FailureOr<AssumeFoldCounts> f = foldAssumedUses(program);
      if (failed(f))
        return signalPassFailure();
      n.foldUses += f->uses + f->intUses;
    }
    if (!clean.empty() && failed(runPipeline(clean, program)))
      return signalPassFailure();
    if (countEdges(region) >= edgesBefore) {
      (void)postconditionFailed("sea-prune-edges",
                                "a round that removed no edge", program);
      return signalPassFailure();
    }
  }
  if (!n.edges)
    return markAllAnalysesPreserved();
  auto i64 = IntegerType::get(ctx, 64);
  SmallVector<NamedAttribute> stats;
  auto stat = [&](StringRef key, uint64_t v) {
    stats.push_back(
        NamedAttribute(StringAttr::get(ctx, key), IntegerAttr::get(i64, v)));
  };
  stat("edges", n.edges);
  stat("by-args", n.byArgs);
  stat("dead-target", n.deadTarget);
  stat("falsified", n.falsified);
  stat("cond-br", n.condBr);
  stat("unreachable", n.unreachable);
  stat("blocks", n.blocks);
  stat("rounds", n.rounds);
  stat("fold-uses", n.foldUses);
  program->setAttr(kPruneEdgesStatsAttr, DictionaryAttr::get(ctx, stats));
  SEA_LOG(logPrune, log::Flow)
      .kv("edges", n.edges)
      .kv("by-args", n.byArgs)
      .kv("dead-target", n.deadTarget)
      .kv("falsified", n.falsified)
      .kv("cond-br", n.condBr)
      .kv("unreachable", n.unreachable)
      .kv("blocks", n.blocks)
      .kv("rounds", n.rounds)
      .kv("fold-uses", n.foldUses);
}

} // namespace

std::unique_ptr<Pass> createPruneEdgesPass(bool fold, bool canonicalize,
                                           bool cse) {
  return std::make_unique<PruneEdgesPass>(fold, canonicalize, cse);
}

std::unique_ptr<Pass> createPruneEdgesPass() {
  return std::make_unique<PruneEdgesPass>();
}

} // namespace sea
