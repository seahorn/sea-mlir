// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AssumeFold.cpp - fold uses of i1 values that checks fix ------------===//
//
// sea-assume-fold (docs/design/AssumeFold.md): after `sea.assume %c` or
// `sea.assert %c`, every use of %c in an op the check properly dominates is
// replaced by the constant 1; likewise for the i1 values the check fixes
// through i1 connectives and comparisons with constants
// (collectAssumedValues: the conjuncts of an `and`, the operand of `xor %c,
// 1`, which is 0, a user `xor %c, 1` of %c, which is 0, ...). An assert is
// an assume for the code it dominates: execution continues past it only if
// it held. canonicalize then folds the constant branches, selects and
// connectives and erases the blocks no path reaches any more;
// sea-merge-asserts drops an assert the fold made 1 and makes one it made 0
// a direct failure.
//
// Exact. Let the check A fix v to b and dominate the user U. Every
// execution that reaches U passed A with its condition 1 (an assume blocks
// the others, a failed assert ends them), so v = b there (each step of
// collectAssumedValues is a bit-vector identity: `and a, b` = 1 implies a =
// b = 1, ...), and SSA values do not change. The program after the fold
// has the same executions with the same values up to their end, and fails
// the same asserts on them; only paths that already end at A lose branches
// they could not take. No premise and no entry of the assumption ledger.
//
// Contract.
//   Accepts: any sea.program (single-region CFG), any number of asserts
//     (it runs before sea-merge-asserts in the lowering).
//   Produces: every use of a fixed value whose owner a check that fixes it
//     properly dominates (in a block reachable from the entry) reads a
//     sea.const of the fixed value (unknown location, at the start of the
//     entry block). Nothing is erased; branches and asserts stay.
//     Checked, on the facts collected again after the rewrite: no such
//     use left.
//   Reports: the counters (kAssumeFoldStatsAttr on the program, only when
//     a use was folded; the lower.assume-fold log): uses (operands
//     replaced) and of them by-assume / by-assert (the check whose fact
//     folded it), values (distinct values with a replaced use), branches /
//     selects (cond_br / select ops whose condition was replaced),
//     asserts-held / asserts-failed (asserts whose condition became 1 / 0),
//     blocks (blocks reachable before and no more once each cond_br with a
//     constant condition takes its one edge).
//
//===----------------------------------------------------------------------===//

#include "sea/Transforms/AssumeFold.h"
#include "PassUtils.h"
#include "sea/Support/Log.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace mlir;

namespace sea {

static log::Tag logAssumeFold("lower.assume-fold", 2,
                              "sea-assume-fold: the counters of a run (1), "
                              "each folded use (2)");

namespace {

bool isI1(Value v) { return v.getType().isInteger(1); }

std::optional<bool> constI1(Value v) {
  APInt k;
  if (isI1(v) && matchPattern(v, m_ConstantInt(&k)))
    return k.isOne();
  return std::nullopt;
}

/// `op`'s operands as (the non-constant one, the constant), when one of the
/// two is an i1 constant.
std::optional<std::pair<Value, bool>> withConstant(Value lhs, Value rhs) {
  if (auto k = constI1(rhs))
    return std::make_pair(lhs, *k);
  if (auto k = constI1(lhs))
    return std::make_pair(rhs, *k);
  return std::nullopt;
}

/// The value of a xor / icmp eq / icmp ne of x with the constant k, given
/// x = b; std::nullopt when op is none of these.
std::optional<bool> decidedBy(Operation *op, bool k, bool b) {
  if (isa<XorOp>(op))
    return b != k;
  if (auto cmp = dyn_cast<ICmpOp>(op)) {
    if (cmp.getPredicate() == ICmpPredicate::eq)
      return b == k;
    if (cmp.getPredicate() == ICmpPredicate::ne)
      return b != k;
  }
  return std::nullopt;
}

/// The operands of an i1 xor / icmp eq|ne with a constant.
std::optional<std::pair<Value, bool>> negationOperand(Operation *op) {
  if (auto x = dyn_cast<XorOp>(op); x && isI1(x.getResult()))
    return withConstant(x.getLhs(), x.getRhs());
  if (auto cmp = dyn_cast<ICmpOp>(op);
      cmp && isI1(cmp.getLhs()) &&
      (cmp.getPredicate() == ICmpPredicate::eq ||
       cmp.getPredicate() == ICmpPredicate::ne))
    return withConstant(cmp.getLhs(), cmp.getRhs());
  return std::nullopt;
}

} // namespace

void collectAssumedValues(ProgramOp program, SmallVectorImpl<AssumedValue> &out,
                          bool withAsserts) {
  program.walk([&](Operation *a) {
    Value cond;
    if (auto as = dyn_cast<AssumeOp>(a))
      cond = as.getCond();
    else if (auto at = dyn_cast<AssertOp>(a); at && withAsserts)
      cond = at.getCond();
    else
      return;
    // per source: value -> fixed truth (the first one; conflicting facts
    // make every point A dominates unreachable, where either is exact)
    llvm::MapVector<Value, bool> fixed;
    SmallVector<std::pair<Value, bool>> todo{{cond, true}};
    while (!todo.empty()) {
      auto [v, b] = todo.pop_back_val();
      if (constI1(v) || !fixed.insert({v, b}).second)
        continue;
      // backward: what v = b says about v's operands
      if (Operation *def = v.getDefiningOp()) {
        if (isa<AndOp>(def) && b) {
          todo.push_back({def->getOperand(0), true});
          todo.push_back({def->getOperand(1), true});
        } else if (isa<OrOp>(def) && !b) {
          todo.push_back({def->getOperand(0), false});
          todo.push_back({def->getOperand(1), false});
        } else if (auto n = negationOperand(def)) {
          // x op k = b; op(x) = (x != k) for xor / ne, (x == k) for eq: x
          // is the one value with decidedBy(def, k, x) = b
          bool x = *decidedBy(def, n->second, true) == b;
          todo.push_back({n->first, x});
        }
      }
      // forward: the i1 users v decides
      for (Operation *user : v.getUsers()) {
        if (user->getNumResults() != 1 || !isI1(user->getResult(0)))
          continue;
        if (isa<AndOp>(user) && !b)
          todo.push_back({user->getResult(0), false});
        else if (isa<OrOp>(user) && b)
          todo.push_back({user->getResult(0), true});
        else if (auto n = negationOperand(user); n && n->first == v)
          todo.push_back({user->getResult(0), *decidedBy(user, n->second, b)});
      }
    }
    for (auto [v, b] : fixed)
      out.push_back({v, b, a});
  });
}

namespace {
/// The assume sea-merge-asserts puts first in the continue block of a merged
/// assert's cond_br (MergeAsserts.cpp): an assume of the cond_br's own
/// condition, in the block the cond_br's true edge enters alone. It is the
/// merge's bookkeeping, not an assume of the input: no fact for the report.
bool isMergeAssertsAssume(Operation *op) {
  auto as = dyn_cast<AssumeOp>(op);
  if (!as || &as->getBlock()->front() != op)
    return false;
  Block *pred = as->getBlock()->getSinglePredecessor();
  auto br = pred ? dyn_cast<CondBrOp>(pred->getTerminator()) : CondBrOp();
  auto n = br ? dyn_cast<NameLoc>(br.getLoc()) : NameLoc();
  return n && n.getName().getValue() == kMergedAssertLocName &&
         br.getTrueDest() == as->getBlock() &&
         br.getCondition() == as.getCond();
}
} // namespace

void findBranchesOnAssumed(ProgramOp program,
                           SmallVectorImpl<BranchOnAssumed> &out) {
  SmallVector<AssumedValue> facts;
  collectAssumedValues(program, facts, /*withAsserts=*/false);
  llvm::erase_if(facts, [](const AssumedValue &f) {
    return isMergeAssertsAssume(f.source);
  });
  DenseMap<Value, unsigned> first;
  for (auto [i, f] : llvm::enumerate(facts))
    first.try_emplace(f.value, i);
  DominanceInfo dom(program);
  program.walk([&](Operation *op) {
    Value c;
    if (auto br = dyn_cast<CondBrOp>(op))
      c = br.getCondition();
    else if (auto sel = dyn_cast<SelectOp>(op))
      c = sel.getCondition();
    else
      return;
    if (!dom.isReachableFromEntry(op->getBlock()))
      return;
    if (auto it = first.find(c); it != first.end())
      out.push_back({op, facts[it->second]});
  });
}

namespace {

/// Blocks reachable from the entry when a cond_br on a sea.const takes its
/// one edge.
llvm::DenseSet<Block *> liveBlocks(ProgramOp program) {
  llvm::DenseSet<Block *> seen;
  Region &r = program->getRegion(0);
  if (r.empty())
    return seen;
  SmallVector<Block *> todo{&r.front()};
  while (!todo.empty()) {
    Block *b = todo.pop_back_val();
    if (!seen.insert(b).second)
      continue;
    Operation *t = b->getTerminator();
    if (auto br = dyn_cast<CondBrOp>(t)) {
      if (auto k = constI1(br.getCondition())) {
        todo.push_back(*k ? br.getTrueDest() : br.getFalseDest());
        continue;
      }
    }
    for (Block *s : t->getSuccessors())
      todo.push_back(s);
  }
  return seen;
}

class AssumeFoldPass
    : public PassWrapper<AssumeFoldPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AssumeFoldPass)

  StringRef getArgument() const final { return "sea-assume-fold"; }
  StringRef getDescription() const final {
    return "Replace the uses of an i1 value that a dominating sea.assume or "
           "sea.assert fixes (the condition, its conjuncts, negations) by "
           "the constant";
  }

  void runOnOperation() override;
};

void AssumeFoldPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  Region &region = program->getRegion(0);
  if (region.empty())
    return markAllAnalysesPreserved();
  SmallVector<AssumedValue> facts;
  collectAssumedValues(program, facts);
  if (facts.empty())
    return markAllAnalysesPreserved();
  DominanceInfo &dom = getAnalysis<DominanceInfo>();
  const llvm::DenseSet<Block *> liveBefore = liveBlocks(program);

  // the constants, made on first use at the start of the entry block; no
  // source location (a replay's last located op stays a program op)
  Value consts[2];
  auto constant = [&](bool b) {
    if (!consts[b]) {
      OpBuilder ob(ctx);
      ob.setInsertionPointToStart(&region.front());
      auto i1 = IntegerType::get(ctx, 1);
      consts[b] = ConstOp::create(ob, ob.getUnknownLoc(), i1,
                                  IntegerAttr::get(i1, APInt(1, b ? 1 : 0)));
    }
    return consts[b];
  };

  uint64_t uses = 0, byAssume = 0, byAssert = 0;
  llvm::SmallPtrSet<Operation *, 16> branchOps, selectOps, assertOps[2];
  llvm::DenseSet<Value> values;
  auto folds = [](DominanceInfo &d, const AssumedValue &f, OpOperand &u) {
    Operation *user = u.getOwner();
    return d.isReachableFromEntry(user->getBlock()) &&
           d.properlyDominates(f.source, user);
  };
  for (const AssumedValue &f : facts) {
    for (OpOperand &u : llvm::make_early_inc_range(f.value.getUses())) {
      if (!folds(dom, f, u))
        continue;
      Operation *user = u.getOwner();
      if (isa<CondBrOp>(user) && u.getOperandNumber() == 0)
        branchOps.insert(user);
      else if (isa<SelectOp>(user) && u.getOperandNumber() == 0)
        selectOps.insert(user);
      else if (isa<AssertOp>(user))
        assertOps[f.truth].insert(user);
      u.set(constant(f.truth));
      ++uses;
      ++(isa<AssertOp>(f.source) ? byAssert : byAssume);
      values.insert(f.value);
      SEA_LOG(logAssumeFold, log::Object)
          .msg("fold")
          .kv("op", user->getName().getStringRef())
          .kv("value", f.truth ? 1 : 0)
          .kv("by", f.source->getName().getStringRef())
          .at(user->getLoc());
    }
  }

  // postcondition, on the facts collected again from the rewritten program
  {
    SmallVector<AssumedValue> again;
    collectAssumedValues(program, again);
    DominanceInfo fresh(program);
    for (const AssumedValue &f : again)
      for (OpOperand &u : f.value.getUses())
        if (folds(fresh, f, u)) {
          (void)postconditionFailed("sea-assume-fold",
                                    "a use of a value a dominating check fixes",
                                    u.getOwner());
          return signalPassFailure();
        }
  }

  if (!uses)
    return markAllAnalysesPreserved();
  const llvm::DenseSet<Block *> liveAfter = liveBlocks(program);
  uint64_t blocks = 0;
  for (Block *b : liveBefore)
    blocks += !liveAfter.contains(b);

  auto i64 = IntegerType::get(ctx, 64);
  SmallVector<NamedAttribute> stats;
  auto stat = [&](StringRef key, uint64_t v) {
    stats.push_back(
        NamedAttribute(StringAttr::get(ctx, key), IntegerAttr::get(i64, v)));
  };
  stat("uses", uses);
  stat("by-assume", byAssume);
  stat("by-assert", byAssert);
  stat("values", values.size());
  stat("branches", branchOps.size());
  stat("selects", selectOps.size());
  stat("asserts-held", assertOps[1].size());
  stat("asserts-failed", assertOps[0].size());
  stat("blocks", blocks);
  program->setAttr(kAssumeFoldStatsAttr, DictionaryAttr::get(ctx, stats));
  SEA_LOG(logAssumeFold, log::Flow)
      .kv("uses", uses)
      .kv("by-assume", byAssume)
      .kv("by-assert", byAssert)
      .kv("values", uint64_t(values.size()))
      .kv("branches", uint64_t(branchOps.size()))
      .kv("selects", uint64_t(selectOps.size()))
      .kv("asserts-held", uint64_t(assertOps[1].size()))
      .kv("asserts-failed", uint64_t(assertOps[0].size()))
      .kv("blocks", blocks);
  // only operands and constants changed: the CFG and dominance stay
  markAnalysesPreserved<DominanceInfo>();
}

} // namespace

std::unique_ptr<Pass> createAssumeFoldPass() {
  return std::make_unique<AssumeFoldPass>();
}

} // namespace sea
