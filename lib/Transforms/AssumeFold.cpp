// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- AssumeFold.cpp - fold uses of i1 values that checks fix ------------===//
//
// sea-assume-fold (docs/design/AssumeFold.md): after `sea.assume %c` or
// `sea.assert %c`, every use of %c in an op the check properly dominates is
// replaced by the constant 1; likewise for the i1 values the check fixes
// through i1 connectives and comparisons with constants
// (collectAssumedValues: the conjuncts of an `and` or of its select form
// `select a, b, false`, the operand of `xor %c,
// 1`, which is 0, a user `xor %c, 1` of %c, which is 0, ...). An assert is
// an assume for the code it dominates: execution continues past it only if
// it held. Edge facts (collectEdgeFacts): the true edge of `sea.cond_br %c`
// fixes %c to 1, the false edge to 0 (same derivations); a fact holds on
// entry of a block when it holds on every executable edge into it, and
// then in every op the block dominates. Integer equalities
// (collectIntFacts): a fact `icmp eq x, K` = 1 or `icmp ne x, K` = 0, x an
// iN with N > 1, fixes x to K in the same region; not addresses or
// pointers (equal addresses are not interchangeable pointers).
// canonicalize then folds the constant
// branches, selects and connectives and erases the blocks no path reaches any
// more; sea-merge-asserts drops an assert the fold made 1 and makes one it made
// 0 a direct failure.
//
// Exact. Let the check A fix v to b and dominate the user U. Every
// execution that reaches U passed A with its condition 1 (an assume blocks
// the others, a failed assert ends them), so v = b there (each step of
// collectAssumedValues is a bit-vector identity: `and a, b` = 1 implies a =
// b = 1, ...), and SSA values do not change. The program after the fold
// has the same executions with the same values up to their end, and fails
// the same asserts on them; only paths that already end at A lose branches
// they could not take. An edge fact is a check on the edge: an execution in
// a block B dominates entered B last on one of its edges, where the fact
// held, and the value was not redefined since. No premise and no entry of
// the assumption ledger.
//
// Contract.
//   Accepts: any sea.program (single-region CFG), any number of asserts
//     (it runs before sea-merge-asserts in the lowering).
//   Produces: every use of a fixed value whose owner a check that fixes it
//     properly dominates, or a block whose edges fix it dominates (in a
//     block reachable from the entry), reads a
//     sea.const of the fixed value (unknown location, at the start of the
//     entry block). Nothing is erased; branches and asserts stay.
//     Likewise every such use of an integer x a fact x == K fixes reads a
//     sea.const K. Checked, on the facts (checks and edges, their integer
//     equalities) collected again after the rewrite: no such use left.
//   Reports: the counters (kAssumeFoldStatsAttr on the program, only when
//     a use was folded; the lower.assume-fold log): uses (operands
//     replaced) and of them by-assume / by-assert / by-edge (the fact that
//     folded it; checks are applied first), int-uses / int-values
//     (operands replaced by an integer equality, distinct integers),
//     values (distinct values with a
//     replaced use), branches / selects (cond_br / select ops whose condition
//     was replaced), asserts-held / asserts-failed (asserts whose condition
//     became 1 / 0), blocks (blocks reachable before and no more once each
//     cond_br with a constant condition takes its one edge).
//
//===----------------------------------------------------------------------===//

#include "sea/Transforms/AssumeFold.h"
#include "PassUtils.h"
#include "sea/Support/Log.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/RegionGraphTraits.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
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

std::optional<Connective> matchConnective(Operation *op) {
  if (!op || op->getNumResults() != 1 || !isI1(op->getResult(0)))
    return std::nullopt;
  if (isa<AndOp, OrOp>(op))
    return Connective{isa<AndOp>(op), op->getOperand(0), op->getOperand(1)};
  auto sel = dyn_cast<SelectOp>(op);
  if (!sel)
    return std::nullopt;
  std::optional<bool> t = constI1(sel.getTrueValue());
  std::optional<bool> f = constI1(sel.getFalseValue());
  if (t && f)
    return std::nullopt;
  if (f && !*f)
    return Connective{true, sel.getCondition(), sel.getTrueValue()};
  if (t && *t)
    return Connective{false, sel.getCondition(), sel.getFalseValue()};
  return std::nullopt;
}

namespace {
/// Adds to `fixed` the values that `cond` = `truth` fixes (with cond):
/// backward through `and` / `or` / negations, forward to the i1 users the
/// value decides. The first truth of a value is kept (conflicting facts make
/// every point they hold at unreachable, where either is exact).
void deriveFixed(Value cond, bool truth, llvm::MapVector<Value, bool> &fixed) {
  SmallVector<std::pair<Value, bool>> todo{{cond, truth}};
  while (!todo.empty()) {
    auto [v, b] = todo.pop_back_val();
    if (constI1(v) || !fixed.insert({v, b}).second)
      continue;
    // backward: what v = b says about v's operands
    if (Operation *def = v.getDefiningOp()) {
      if (auto k = matchConnective(def)) {
        // `and` true / `or` false fixes both operands to the same truth
        if (k->isAnd == b) {
          todo.push_back({k->lhs, b});
          todo.push_back({k->rhs, b});
        }
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
      if (auto k = matchConnective(user)) {
        // `and` with a false operand is false, `or` with a true one true
        if (k->isAnd != b && (k->lhs == v || k->rhs == v))
          todo.push_back({user->getResult(0), b});
      } else if (auto n = negationOperand(user); n && n->first == v)
        todo.push_back({user->getResult(0), *decidedBy(user, n->second, b)});
    }
  }
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
    // per source: value -> fixed truth
    llvm::MapVector<Value, bool> fixed;
    deriveFixed(cond, true, fixed);
    for (auto [v, b] : fixed)
      out.push_back({v, b, a});
  });
}

void collectEdgeFacts(ProgramOp program, DominanceInfo &dom,
                      ArrayRef<AssumedValue> checks,
                      SmallVectorImpl<AssumedValue> &out) {
  Region &r = program->getRegion(0);
  if (r.empty() || r.hasOneBlock())
    return;
  DenseMap<Value, SmallVector<const AssumedValue *, 1>> checksOf;
  for (const AssumedValue &f : checks)
    if (!f.block)
      checksOf[f.value].push_back(&f);
  // what an edge fixes: a cond_br's condition on its true / false edge
  // (none when both edges enter the same block, or for any other edge)
  DenseMap<std::pair<Operation *, unsigned>, llvm::MapVector<Value, bool>>
      edgeMemo;
  auto edgeFixes =
      [&](BlockOperand &e) -> const llvm::MapVector<Value, bool> & {
    Operation *t = e.getOwner();
    auto key = std::make_pair(t, e.getOperandNumber());
    auto [it, fresh] = edgeMemo.try_emplace(key);
    if (fresh)
      if (auto br = dyn_cast<CondBrOp>(t);
          br && br.getTrueDest() != br.getFalseDest())
        deriveFixed(br.getCondition(), e.get() == br.getTrueDest(), it->second);
    return it->second;
  };
  // block -> the values fixed on entry (filled in reverse post-order: a
  // block's forward predecessors first; a back edge's source not yet
  // filled has fewer facts, which only drops candidates)
  DenseMap<Block *, llvm::MapVector<Value, bool>> entryFacts;
  // v = b at the end of `p`: a check that dominates its terminator, or the
  // entry facts of a block that dominates p
  auto holdsAtEnd = [&](Block *p, Value v, bool b) {
    if (auto it = checksOf.find(v); it != checksOf.end())
      for (const AssumedValue *f : it->second)
        if (f->truth == b &&
            dom.properlyDominates(f->source, p->getTerminator()))
          return true;
    for (DominanceInfoNode *n = dom.getNode(p); n; n = n->getIDom()) {
      auto it = entryFacts.find(n->getBlock());
      if (it == entryFacts.end())
        continue;
      auto f = it->second.find(v);
      if (f != it->second.end() && f->second == b)
        return true;
    }
    return false;
  };
  for (Block *s : llvm::ReversePostOrderTraversal<Block *>(&r.front())) {
    // the edges into s that can execute
    SmallVector<BlockOperand *, 4> edges;
    for (BlockOperand &e : s->getUses())
      if (dom.isReachableFromEntry(e.getOwner()->getBlock()))
        edges.push_back(&e);
    if (edges.empty())
      continue;
    // candidates: what some edge fixes (the first edge that does is the
    // source of the fact); kept if it holds on every edge into s
    llvm::MapVector<std::pair<Value, unsigned>, Operation *> candidates;
    for (BlockOperand *e : edges)
      for (auto [v, b] : edgeFixes(*e))
        candidates.insert({{v, unsigned(b)}, e->getOwner()});
    if (candidates.empty())
      continue;
    llvm::MapVector<Value, bool> &fixed = entryFacts[s];
    for (auto [vb, source] : candidates) {
      Value v = vb.first;
      bool b = vb.second;
      bool all = llvm::all_of(edges, [&](BlockOperand *e) {
        const auto &on = edgeFixes(*e);
        auto f = on.find(v);
        if (f != on.end())
          return f->second == b;
        return edges.size() > 1 && holdsAtEnd(e->getOwner()->getBlock(), v, b);
      });
      if (all && fixed.insert({v, b}).second)
        out.push_back({v, b, source, s});
    }
  }
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

/// An integer equality `value = k` (value an iN, N > 1) that the i1 fact
/// `from` gives (D7 of docs/design/FactFolds.md): `icmp eq x, K` = 1 or
/// `icmp ne x, K` = 0, K a constant on either side. Its region is the
/// fact's. Integers only: sea.icmp takes integers, and an address or
/// pointer compare (sea.addr.cmp, sea.ptr.cmp) is never read: equal
/// addresses do not make two pointers interchangeable (PtrSort.md).
struct IntFact {
  Value value;
  APInt k;
  const AssumedValue *from;
};

void collectIntFacts(ArrayRef<AssumedValue> facts,
                     SmallVectorImpl<IntFact> &out) {
  for (const AssumedValue &f : facts) {
    auto cmp = f.value.getDefiningOp<ICmpOp>();
    if (!cmp)
      continue;
    ICmpPredicate p = cmp.getPredicate();
    if (!(p == ICmpPredicate::eq && f.truth) &&
        !(p == ICmpPredicate::ne && !f.truth))
      continue;
    APInt k;
    Value x;
    if (matchPattern(cmp.getRhs(), m_ConstantInt(&k)))
      x = cmp.getLhs();
    else if (matchPattern(cmp.getLhs(), m_ConstantInt(&k)))
      x = cmp.getRhs();
    else
      continue;
    auto t = dyn_cast<IntegerType>(x.getType());
    if (!t || t.getWidth() < 2 || matchPattern(x, m_Constant()))
      continue; // i1: D3 already; constant: the folders'
    out.push_back({x, k, &f});
  }
}

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
           "sea.assert, or the edges into a dominating block (cond_br), fix "
           "(the condition, its conjuncts, negations) by the constant";
  }

  void runOnOperation() override;
};

} // namespace

FailureOr<AssumeFoldCounts> foldAssumedUses(ProgramOp program) {
  MLIRContext *ctx = program->getContext();
  Region &region = program->getRegion(0);
  AssumeFoldCounts n;
  if (region.empty())
    return n;
  DominanceInfo dom(program);
  // the facts of the program as it is: the checks' first, then the edges'
  // (a use both fix counts as by its check)
  auto collect = [&](DominanceInfo &d, SmallVectorImpl<AssumedValue> &out) {
    collectAssumedValues(program, out);
    SmallVector<AssumedValue> edges;
    collectEdgeFacts(program, d, out, edges);
    out.append(edges);
  };
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
  // integer constants, likewise in the entry block, one per type and value
  DenseMap<Attribute, Value> intConsts;
  auto intConstant = [&](Type t, const APInt &k) {
    auto attr = IntegerAttr::get(t, k);
    Value &c = intConsts[attr];
    if (!c) {
      OpBuilder ob(ctx);
      ob.setInsertionPointToStart(&region.front());
      c = ConstOp::create(ob, ob.getUnknownLoc(), t, attr);
    }
    return c;
  };

  uint64_t uses = 0, byAssume = 0, byAssert = 0, byEdge = 0, intUses = 0;
  llvm::DenseSet<Value> intValues;
  llvm::SmallPtrSet<Operation *, 16> branchOps, selectOps, assertOps[2];
  llvm::DenseSet<Value> values;
  auto folds = [](DominanceInfo &d, const AssumedValue &f, OpOperand &u) {
    Operation *user = u.getOwner();
    if (!d.isReachableFromEntry(user->getBlock()))
      return false;
    return f.block ? d.dominates(f.block, user->getBlock())
                   : d.properlyDominates(f.source, user);
  };

  // Rounds: a substitution can make a new fact (`icmp eq y, x` with x
  // replaced by K is the equality y == K; an i1 fact's derivation can pass
  // the new constant), so the facts are collected again until a round
  // replaces nothing. Each replacement turns an operand that is not a
  // sea.const into one, and no fact is about a constant, so there are at
  // most as many replacements as such operands, and at most one round more
  // than that (checked).
  uint64_t bound = 1;
  program.walk([&](Operation *op) {
    for (Value v : op->getOperands())
      bound += !matchPattern(v, m_Constant());
  });
  uint64_t rounds = 0;
  for (;; ++rounds) {
    if (rounds > bound) {
      return postconditionFailed(
          "sea-assume-fold", "more rounds than operands to replace", program);
    }
    const uint64_t before = uses + intUses;
    SmallVector<AssumedValue> facts;
    collect(dom, facts);
    // the integer equalities of these facts (before this round's rewrite)
    SmallVector<IntFact> intFacts;
    collectIntFacts(facts, intFacts);
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
        ++(f.block ? byEdge : isa<AssertOp>(f.source) ? byAssert : byAssume);
        values.insert(f.value);
        SEA_LOG(logAssumeFold, log::Object)
            .msg("fold")
            .kv("op", user->getName().getStringRef())
            .kv("value", f.truth ? 1 : 0)
            .kv("by", f.block ? StringRef("edge")
                              : f.source->getName().getStringRef())
            .at(user->getLoc());
      }
    }
    // integer equalities (F3): each use of x in the region reads K. The
    // icmp that makes the fact is not in its region (it defines the fact's
    // value, which dominates the source); prints and block-argument
    // operands are plain uses; an op with nsw / nuw keeps its flags (an
    // equal operand; such ops do not fold)
    for (const IntFact &f : intFacts) {
      for (OpOperand &u : llvm::make_early_inc_range(f.value.getUses())) {
        if (!folds(dom, *f.from, u))
          continue;
        Operation *user = u.getOwner();
        u.set(intConstant(f.value.getType(), f.k));
        ++intUses;
        intValues.insert(f.value);
        SEA_LOG(logAssumeFold, log::Object)
            .msg("fold-int")
            .kv("op", user->getName().getStringRef())
            .kv("by", f.from->block ? StringRef("edge")
                                    : f.from->source->getName().getStringRef())
            .at(user->getLoc());
      }
    }
    if (uses + intUses == before)
      break;
  }

  // postcondition, on the facts collected again from the rewritten program
  {
    SmallVector<AssumedValue> again;
    collectAssumedValues(program, again);
    DominanceInfo fresh(program);
    {
      SmallVector<AssumedValue> edges;
      collectEdgeFacts(program, fresh, again, edges);
      again.append(edges);
    }
    for (const AssumedValue &f : again)
      for (OpOperand &u : f.value.getUses())
        if (folds(fresh, f, u)) {
          return postconditionFailed(
              "sea-assume-fold", "a use of a value a dominating check fixes",
              u.getOwner());
        }
    SmallVector<IntFact> intAgain;
    collectIntFacts(again, intAgain);
    for (const IntFact &f : intAgain)
      for (OpOperand &u : f.value.getUses())
        if (folds(fresh, *f.from, u)) {
          return postconditionFailed(
              "sea-assume-fold",
              "a use of an integer a dominating equality fixes", u.getOwner());
        }
  }

  n.uses = uses;
  n.byAssume = byAssume;
  n.byAssert = byAssert;
  n.byEdge = byEdge;
  n.values = values.size();
  n.branches = branchOps.size();
  n.selects = selectOps.size();
  n.assertsHeld = assertOps[1].size();
  n.assertsFailed = assertOps[0].size();
  n.intUses = intUses;
  n.intValues = intValues.size();
  n.rounds = rounds;
  if (!uses && !intUses)
    return n;
  const llvm::DenseSet<Block *> liveAfter = liveBlocks(program);
  for (Block *b : liveBefore)
    n.blocks += !liveAfter.contains(b);
  return n;
}

namespace {

void AssumeFoldPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  FailureOr<AssumeFoldCounts> r = foldAssumedUses(program);
  if (failed(r))
    return signalPassFailure();
  const AssumeFoldCounts &n = *r;
  if (!n.uses && !n.intUses)
    return markAllAnalysesPreserved();
  auto i64 = IntegerType::get(ctx, 64);
  SmallVector<NamedAttribute> stats;
  auto stat = [&](StringRef key, uint64_t v) {
    stats.push_back(
        NamedAttribute(StringAttr::get(ctx, key), IntegerAttr::get(i64, v)));
  };
  stat("uses", n.uses);
  stat("by-assume", n.byAssume);
  stat("by-assert", n.byAssert);
  stat("by-edge", n.byEdge);
  stat("values", n.values);
  stat("branches", n.branches);
  stat("selects", n.selects);
  stat("asserts-held", n.assertsHeld);
  stat("asserts-failed", n.assertsFailed);
  stat("blocks", n.blocks);
  stat("int-uses", n.intUses);
  stat("int-values", n.intValues);
  stat("rounds", n.rounds);
  program->setAttr(kAssumeFoldStatsAttr, DictionaryAttr::get(ctx, stats));
  SEA_LOG(logAssumeFold, log::Flow)
      .kv("uses", n.uses)
      .kv("by-assume", n.byAssume)
      .kv("by-assert", n.byAssert)
      .kv("by-edge", n.byEdge)
      .kv("values", n.values)
      .kv("branches", n.branches)
      .kv("selects", n.selects)
      .kv("asserts-held", n.assertsHeld)
      .kv("asserts-failed", n.assertsFailed)
      .kv("blocks", n.blocks)
      .kv("int-uses", n.intUses)
      .kv("int-values", n.intValues)
      .kv("rounds", n.rounds);
  // only operands and constants changed: the CFG and dominance stay
  markAnalysesPreserved<DominanceInfo>();
}

} // namespace

std::unique_ptr<Pass> createAssumeFoldPass() {
  return std::make_unique<AssumeFoldPass>();
}

} // namespace sea
