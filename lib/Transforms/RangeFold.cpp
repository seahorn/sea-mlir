// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- RangeFold.cpp - fold values that integer ranges settle -------------===//
//
// sea-range-fold (docs/design/RangeFold.md): a sea.icmp, an i1 sea.and /
// sea.or / sea.xor, or the condition of a sea.select whose value the integer
// ranges of its operands settle, at its position, is folded: the icmp / i1
// op is replaced by the constant, the select by the arm it takes.
//
// Ranges are computed at fold time, per candidate (value tracking, not a
// dataflow analysis: the facts depend on the candidate's position): the
// range of a value is
//   - the range its op's InferIntRangeInterface gives from the ranges of
//     its operands, computed the same way, recursively (sea's value ops:
//     lib/Dialect/Sea/SeaIntRanges.cpp), up to `max-depth` ops deep; a
//     leaf (a load, a nondet, a block argument, an op without the
//     interface, or the depth cap) has the full range;
//   - intersected, at every value of the walk, with the facts on that value
//     whose sea.assume dominates the candidate: same block, before it; or
//     a block that dominates its block (mlir::DominanceInfo). A fact from
//     anywhere else is never used (a non-dominating fact can make a
//     verdict wrong).
// A fact is a simple bound: `sea.assume c` with c a sea.icmp of a value x
// and a constant (either order, any predicate; `ne` only at the ends of
// x's range, else it is a hole and skipped), or an i1 sea.and (nested) of
// such conjuncts. A bound on sext(y) / zext(y) is also one on y (inverted
// exactly, RangeFold.h). Anything else is not a fact; each such conjunct is
// counted (facts-skipped). No memory reasoning: a load is a leaf, refined
// only by facts on that same SSA value.
//
// Why a fold is sound. Let the assume A dominate the candidate P. Every
// execution that reaches P passed A with c = 1, and SSA values do not
// change, so at P the fact holds of x. The range of a value at P is then a
// superset of the values it can have at P (the transfer functions are
// sound for sea's total semantics, unittests/IntRanges), so a range of one
// value is its value at P in every execution reaching P; every use of P's
// result is dominated by P. No circularity: a fact from A is used only at
// points A dominates, while A's own condition is computed before A.
//
// The range walk uses the nsw / nuw flags of add / sub / mul / shl when an
// op carries them (the upstream transfer functions do): the flags are
// binding semantics (a front end that does not trust them strips them),
// which the VC encoding over-approximates by wrapping. A fold whose walk met
// a flagged op is counted (wrap-flags, informational).
//
// Contract.
//   Accepts: any sea.program (single-region CFG).
//   Produces (mode=fold): every candidate whose range is one value is
//     replaced (the icmp / i1 op erased when dead; the select's uses take
//     its arm). Checked: no folded value keeps a use.
//   Produces (mode=check): for every such candidate, right after it (the
//     select: right before it), `sea.assert (icmp eq v, k)` of the settled
//     value k; nothing is folded; the program's own asserts are dropped
//     (reach-site markers kept, as sea-check-align). unsat = every fold
//     the pass makes is valid. Checked: one assert per fold, none of the
//     program's.
//   Reports: the counters (kRangeFoldStatsAttr on the program; the
//     lower.range-fold log).
//
//===----------------------------------------------------------------------===//

#include "sea/Transforms/RangeFold.h"
#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Log.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace mlir;

namespace sea {

static log::Tag
    logRangeFold("lower.range-fold", 2,
                 "sea-range-fold: the counters of a run (1), each fold (2)");

bool isEmptyRange(const ConstantIntRanges &r) {
  return r.umin().ugt(r.umax()) || r.smin().sgt(r.smax());
}

ConstantIntRanges tightenRange(const ConstantIntRanges &r) {
  if (isEmptyRange(r))
    return r;
  ConstantIntRanges out = r;
  // signed inside one half: the same interval unsigned
  if (out.smin().isNonNegative() || out.smax().isNegative())
    out = out.intersection(
        ConstantIntRanges::fromUnsigned(out.smin(), out.smax()));
  if (isEmptyRange(out))
    return out;
  // unsigned inside one half: the same interval signed
  if (out.umax().isNonNegative() || out.umin().isNegative())
    out =
        out.intersection(ConstantIntRanges::fromSigned(out.umin(), out.umax()));
  return out;
}

ConstantIntRanges emptyRange(unsigned width) {
  return ConstantIntRanges(APInt(width, 1), APInt::getZero(width),
                           APInt::getSignedMaxValue(width),
                           APInt::getSignedMinValue(width));
}

ICmpPredicate swapPredicate(ICmpPredicate p) {
  switch (p) {
  case ICmpPredicate::eq:
  case ICmpPredicate::ne:
    return p;
  case ICmpPredicate::slt:
    return ICmpPredicate::sgt;
  case ICmpPredicate::sle:
    return ICmpPredicate::sge;
  case ICmpPredicate::sgt:
    return ICmpPredicate::slt;
  case ICmpPredicate::sge:
    return ICmpPredicate::sle;
  case ICmpPredicate::ult:
    return ICmpPredicate::ugt;
  case ICmpPredicate::ule:
    return ICmpPredicate::uge;
  case ICmpPredicate::ugt:
    return ICmpPredicate::ult;
  case ICmpPredicate::uge:
    return ICmpPredicate::ule;
  }
  llvm_unreachable("unknown icmp predicate");
}

std::optional<ConstantIntRanges> boundOfPredicate(ICmpPredicate p,
                                                  const APInt &c) {
  unsigned w = c.getBitWidth();
  APInt umin = APInt::getZero(w), umax = APInt::getMaxValue(w);
  APInt smin = APInt::getSignedMinValue(w), smax = APInt::getSignedMaxValue(w);
  auto u = [](const APInt &lo, const APInt &hi) {
    return ConstantIntRanges::fromUnsigned(lo, hi);
  };
  auto s = [](const APInt &lo, const APInt &hi) {
    return ConstantIntRanges::fromSigned(lo, hi);
  };
  switch (p) {
  case ICmpPredicate::eq:
    return ConstantIntRanges::constant(c);
  case ICmpPredicate::ne: {
    // only a bound at an end of the unsigned or signed range is an interval
    ConstantIntRanges r = ConstantIntRanges::maxRange(w);
    bool any = false;
    if (c == umin) {
      r = r.intersection(u(c + 1, umax));
      any = true;
    } else if (c == umax) {
      r = r.intersection(u(umin, c - 1));
      any = true;
    }
    if (c == smin) {
      r = r.intersection(s(c + 1, smax));
      any = true;
    } else if (c == smax) {
      r = r.intersection(s(smin, c - 1));
      any = true;
    }
    if (!any)
      return std::nullopt;
    return r;
  }
  case ICmpPredicate::ult:
    return c.isZero() ? emptyRange(w) : u(umin, c - 1);
  case ICmpPredicate::ule:
    return u(umin, c);
  case ICmpPredicate::ugt:
    return c.isMaxValue() ? emptyRange(w) : u(c + 1, umax);
  case ICmpPredicate::uge:
    return u(c, umax);
  case ICmpPredicate::slt:
    return c.isMinSignedValue() ? emptyRange(w) : s(smin, c - 1);
  case ICmpPredicate::sle:
    return s(smin, c);
  case ICmpPredicate::sgt:
    return c.isMaxSignedValue() ? emptyRange(w) : s(c + 1, smax);
  case ICmpPredicate::sge:
    return s(c, smax);
  }
  llvm_unreachable("unknown icmp predicate");
}

ConstantIntRanges rangeBeforeExt(const ConstantIntRanges &r, bool isSigned,
                                 unsigned n) {
  unsigned w = r.umin().getBitWidth();
  assert(n < w && "an extension widens");
  if (isEmptyRange(r))
    return emptyRange(n);
  // ext is monotone and injective in both orders, so the preimage of an
  // interval is an interval: [the least y with ext(y) >= lo, the greatest
  // y with ext(y) <= hi], computed on the image [imgLo, imgHi] ∪ ... below
  APInt nSMin = APInt::getSignedMinValue(n).sext(w);
  APInt nSMax = APInt::getSignedMaxValue(n).sext(w);
  APInt nUMax = APInt::getMaxValue(n).zext(w);
  APInt ulo, uhi, slo, shi; // n-bit bounds
  if (isSigned) {
    // signed order: the image of sext is [smin_n, smax_n] (as w-bit signed)
    if (r.smax().slt(nSMin) || r.smin().sgt(nSMax))
      return emptyRange(n);
    slo = (r.smin().slt(nSMin) ? nSMin : r.smin()).trunc(n);
    shi = (r.smax().sgt(nSMax) ? nSMax : r.smax()).trunc(n);
    // unsigned order: the image is [0, smax_n] ∪ [2^w - 2^(n-1), 2^w - 1]
    // (y unsigned in [0, 2^(n-1)) then [2^(n-1), 2^n)), increasing
    APInt hiImgLo = nSMin; // 2^w - 2^(n-1) as an unsigned w-bit value
    const APInt &lo = r.umin(), &hi = r.umax();
    if (lo.ule(nSMax))
      ulo = lo.trunc(n);
    else if (lo.ule(hiImgLo))
      ulo = APInt::getSignedMinValue(n); // the first y of the upper part
    else
      ulo = lo.trunc(n);
    if (hi.uge(hiImgLo))
      uhi = hi.trunc(n);
    else if (hi.uge(nSMax))
      uhi = APInt::getSignedMaxValue(n); // the last y of the lower part
    else
      uhi = hi.trunc(n);
  } else {
    // unsigned order: the image of zext is [0, 2^n - 1]
    if (r.umin().ugt(nUMax))
      return emptyRange(n);
    ulo = r.umin().trunc(n);
    uhi = (r.umax().ugt(nUMax) ? nUMax : r.umax()).trunc(n);
    // signed order: the image is [0, 2^n - 1] too (w > n), as w-bit signed
    // values, and zext(y) is increasing in y unsigned
    APInt zero = APInt::getZero(w);
    if (r.smax().slt(zero) || r.smin().sgt(nUMax))
      return emptyRange(n);
    APInt lo = r.smin().slt(zero) ? zero : r.smin();
    APInt hi = r.smax().sgt(nUMax) ? nUMax : r.smax();
    ulo = llvm::APIntOps::umax(ulo, lo.trunc(n));
    uhi = llvm::APIntOps::umin(uhi, hi.trunc(n));
    if (ulo.ugt(uhi))
      return emptyRange(n);
    return ConstantIntRanges::fromUnsigned(ulo, uhi);
  }
  if (ulo.ugt(uhi) || slo.sgt(shi))
    return emptyRange(n);
  ConstantIntRanges out =
      tightenRange(ConstantIntRanges::fromUnsigned(ulo, uhi).intersection(
          ConstantIntRanges::fromSigned(slo, shi)));
  return isEmptyRange(out) ? emptyRange(n) : out;
}

namespace {

struct Fact {
  Operation *assume;
  ConstantIntRanges range;
};

struct Counters {
  uint64_t icmp = 0, boolean = 0, select = 0, facts = 0, factsSkipped = 0,
           factsUsed = 0, noFacts = 0, depthCap = 0, empty = 0, wrapFlags = 0,
           queries = 0;
};

bool isWrapFlagged(Operation *op) {
  if (auto o = dyn_cast<AddOp>(op))
    return o.getNsw() || o.getNuw();
  if (auto o = dyn_cast<SubOp>(op))
    return o.getNsw() || o.getNuw();
  if (auto o = dyn_cast<MulOp>(op))
    return o.getNsw() || o.getNuw();
  if (auto o = dyn_cast<ShlOp>(op))
    return o.getNsw() || o.getNuw();
  return false;
}

bool isI1(Value v) { return v.getType().isInteger(1); }

/// The facts of a program: per value, the simple bounds and their assumes.
class FactIndex {
public:
  explicit FactIndex(ProgramOp program, Counters &n) : n(n) {
    program.walk([&](AssumeOp a) { add(a); });
  }
  ArrayRef<Fact> of(Value v) const {
    auto it = facts.find(v);
    return it == facts.end() ? ArrayRef<Fact>() : ArrayRef<Fact>(it->second);
  }

private:
  void add(AssumeOp a) {
    SmallVector<Value> todo{a.getCond()};
    while (!todo.empty()) {
      Value c = todo.pop_back_val();
      if (auto andOp = c.getDefiningOp<AndOp>(); andOp && isI1(c)) {
        todo.push_back(andOp.getLhs());
        todo.push_back(andOp.getRhs());
        continue;
      }
      APInt k;
      if (matchPattern(c, m_ConstantInt(&k)) && k.isOne())
        continue; // `and true, ...`: no constraint
      if (!bound(c, a))
        ++n.factsSkipped;
    }
  }
  bool bound(Value c, AssumeOp a) {
    auto cmp = c.getDefiningOp<ICmpOp>();
    if (!cmp)
      return false;
    APInt k;
    Value x;
    ICmpPredicate p = cmp.getPredicate();
    if (matchPattern(cmp.getRhs(), m_ConstantInt(&k)))
      x = cmp.getLhs();
    else if (matchPattern(cmp.getLhs(), m_ConstantInt(&k))) {
      x = cmp.getRhs();
      p = swapPredicate(p);
    } else {
      return false;
    }
    if (matchPattern(x, m_Constant()))
      return false; // constant vs constant: the folders' job
    std::optional<ConstantIntRanges> r = boundOfPredicate(p, k);
    if (!r)
      return false;
    ++n.facts;
    facts[x].push_back({a, *r});
    // exactly inverted through extensions
    while (Operation *def = x.getDefiningOp()) {
      bool isSigned = isa<SExtOp>(def);
      if (!isSigned && !isa<ZExtOp>(def))
        break;
      Value y = def->getOperand(0);
      *r = rangeBeforeExt(*r, isSigned, bitWidthOf(y.getType()));
      facts[y].push_back({a, *r});
      x = y;
    }
    return true;
  }

  Counters &n;
  DenseMap<Value, SmallVector<Fact, 2>> facts;
};

/// The range of values at one point (a candidate), memoized for that point.
class RangeQuery {
public:
  RangeQuery(const FactIndex &facts, DominanceInfo &dom, Operation *point,
             unsigned maxDepth)
      : facts(facts), dom(dom), point(point), maxDepth(maxDepth) {}

  ConstantIntRanges range(Value v, unsigned depth = 0) {
    if (auto it = memo.find(v); it != memo.end())
      return it->second;
    const unsigned capsBefore = capHits;
    unsigned w = bitWidthOf(v.getType());
    ConstantIntRanges r = ConstantIntRanges::maxRange(w);
    Operation *def = v.getDefiningOp();
    auto iface = dyn_cast_if_present<InferIntRangeInterface>(def);
    if (iface && def->getNumResults() == 1 &&
        llvm::all_of(def->getOperandTypes(),
                     [](Type t) { return isa<IntegerType>(t); })) {
      if (depth >= maxDepth) {
        ++capHits;
      } else {
        SmallVector<ConstantIntRanges> args;
        bool anyEmpty = false;
        for (Value o : def->getOperands()) {
          args.push_back(range(o, depth + 1));
          anyEmpty |= isEmptyRange(args.back());
        }
        if (anyEmpty) {
          r = emptyRange(w);
        } else {
          iface.inferResultRanges(args,
                                  [&](Value res, const ConstantIntRanges &cr) {
                                    if (res == v)
                                      r = tightenRange(cr);
                                  });
          if (isWrapFlagged(def))
            wrapFlags = true;
        }
      }
    }
    for (const Fact &f : facts.of(v)) {
      if (!dom.properlyDominates(f.assume, point))
        continue;
      ConstantIntRanges n = tightenRange(r.intersection(f.range));
      if (!(n == r)) {
        ++factsUsed;
        r = n;
      }
    }
    if (isEmptyRange(r))
      empty = true;
    // a range the depth cap cut is not memoized: a shallower visit of the
    // same value in this query computes it again, past the cap
    if (capHits == capsBefore)
      memo.try_emplace(v, r);
    return r;
  }

  bool empty = false, wrapFlags = false;
  unsigned factsUsed = 0, capHits = 0;

private:
  const FactIndex &facts;
  DominanceInfo &dom;
  Operation *point;
  unsigned maxDepth;
  DenseMap<Value, ConstantIntRanges> memo;
};

class RangeFoldPass
    : public PassWrapper<RangeFoldPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(RangeFoldPass)

  RangeFoldPass() = default;
  RangeFoldPass(const RangeFoldPass &other) : PassWrapper(other) {}
  RangeFoldPass(RangeFoldMode m, unsigned depth) {
    mode = m == RangeFoldMode::Check ? "check" : "fold";
    maxDepth = depth;
  }

  StringRef getArgument() const final { return "sea-range-fold"; }
  StringRef getDescription() const final {
    return "Fold sea.icmp / i1 ops / select conditions that the integer "
           "ranges settle, with the facts of dominating sea.assume ops "
           "(mode=check: assert each fold instead)";
  }

  void runOnOperation() override;

  Option<std::string> mode{
      *this, "mode",
      llvm::cl::desc("fold (default) or check (assert every fold, drop the "
                     "program's asserts)"),
      llvm::cl::init("fold")};
  Option<unsigned> maxDepth{
      *this, "max-depth",
      llvm::cl::desc("Ops deep the range walk goes from a candidate (untuned; "
                     "default 12)"),
      llvm::cl::init(12)};
};

bool allConstantOperands(Operation *op) {
  return llvm::all_of(op->getOperands(),
                      [](Value v) { return matchPattern(v, m_Constant()); });
}

void RangeFoldPass::runOnOperation() {
  if (mode != "fold" && mode != "check") {
    getOperation().emitError("sea-range-fold: mode must be fold or check");
    return signalPassFailure();
  }
  const bool check = mode == "check";
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  Counters n;
  FactIndex facts(program, n);
  DominanceInfo &dom = getAnalysis<DominanceInfo>();

  SmallVector<AssertOp> original;
  if (check)
    program.walk([&](AssertOp a) { original.push_back(a); });
  SmallVector<Operation *> candidates;
  program.walk([&](Operation *op) {
    if (isa<ICmpOp>(op) ||
        (isa<AndOp, OrOp, XorOp>(op) && isI1(op->getResult(0))) ||
        isa<SelectOp>(op))
      candidates.push_back(op);
  });

  // First decide every candidate on the unmodified program, so that
  // mode=fold and mode=check cover the same set (a fold made first would
  // change what later candidates see).
  struct Decision {
    Operation *op;
    Value target;
    APInt k;
  };
  SmallVector<Decision> decisions;
  for (Operation *op : candidates) {
    if (!dom.isReachableFromEntry(op->getBlock()))
      continue;
    auto sel = dyn_cast<SelectOp>(op);
    Value target = sel ? Value(sel.getCondition()) : op->getResult(0);
    // all constant: the folders' (canonicalize's) job, not a range fold
    if (sel ? matchPattern(target, m_Constant()) : allConstantOperands(op))
      continue;
    ++n.queries;
    RangeQuery q(facts, dom, op, maxDepth);
    ConstantIntRanges r = q.range(target);
    if (q.capHits)
      ++n.depthCap;
    if (q.empty) {
      ++n.empty;
      continue;
    }
    std::optional<APInt> k = r.getConstantValue();
    if (!k)
      continue;
    StringRef kind = sel ? "select" : isa<ICmpOp>(op) ? "icmp" : "bool";
    ++(sel ? n.select : isa<ICmpOp>(op) ? n.icmp : n.boolean);
    n.factsUsed += q.factsUsed;
    if (!q.factsUsed)
      ++n.noFacts;
    if (q.wrapFlags)
      ++n.wrapFlags;
    SEA_LOG(logRangeFold, log::Object)
        .msg(check ? "check" : "fold")
        .kv("kind", kind)
        .kv("value", k->getZExtValue())
        .kv("facts-used", q.factsUsed)
        .kv("wrap-flags", q.wrapFlags ? 1 : 0)
        .at(op->getLoc());
    decisions.push_back({op, target, *k});
  }

  // Then apply the decisions.
  OpBuilder b(ctx);
  SmallVector<Operation *> folded;
  unsigned checks = 0;
  for (Decision &d : decisions) {
    Operation *op = d.op;
    Value target = d.target;
    const APInt *k = &d.k;
    auto sel = dyn_cast<SelectOp>(op);
    Location loc = op->getLoc();
    if (check) {
      if (sel)
        b.setInsertionPoint(op);
      else
        b.setInsertionPointAfter(op);
      Type t = target.getType();
      Value kv = ConstOp::create(b, loc, t, IntegerAttr::get(t, *k));
      Value eq = ICmpOp::create(b, loc, ICmpPredicate::eq, target, kv);
      AssertOp::create(b, loc, eq);
      ++checks;
      continue;
    }
    if (sel) {
      sel.getResult().replaceAllUsesWith(k->isOne() ? sel.getTrueValue()
                                                    : sel.getFalseValue());
    } else {
      b.setInsertionPoint(op);
      Type t = target.getType();
      target.replaceAllUsesWith(
          ConstOp::create(b, loc, t, IntegerAttr::get(t, *k)));
    }
    folded.push_back(op);
  }
  for (Operation *op : llvm::reverse(folded)) {
    if (!op->use_empty()) {
      (void)postconditionFailed("sea-range-fold", "a use of a folded value",
                                op);
      return signalPassFailure();
    }
    op->erase();
  }
  if (check) {
    OpBuilder mb(ctx);
    for (AssertOp a : original) {
      if (Attribute site = a->getAttr(kReachSiteAttr)) {
        mb.setInsertionPoint(a);
        createReachSiteMarker(mb, a.getLoc(), a->getName().getStringRef(),
                              site);
      }
      a.erase();
    }
    unsigned asserts = 0;
    program.walk([&](AssertOp) { ++asserts; });
    if (asserts != checks) {
      (void)postconditionFailed("sea-range-fold",
                                "a number of asserts other than its checks",
                                program);
      return signalPassFailure();
    }
  }

  auto i64 = IntegerType::get(ctx, 64);
  SmallVector<NamedAttribute> stats;
  auto stat = [&](StringRef key, uint64_t v) {
    stats.push_back(
        NamedAttribute(StringAttr::get(ctx, key), IntegerAttr::get(i64, v)));
  };
  stat("icmp", n.icmp);
  stat("bool", n.boolean);
  stat("select", n.select);
  stat("checks", checks);
  stat("queries", n.queries);
  stat("facts", n.facts);
  stat("facts-skipped", n.factsSkipped);
  stat("facts-used", n.factsUsed);
  stat("no-facts", n.noFacts);
  stat("depth-cap", n.depthCap);
  stat("empty", n.empty);
  stat("wrap-flags", n.wrapFlags);
  program->setAttr(kRangeFoldStatsAttr, DictionaryAttr::get(ctx, stats));
  SEA_LOG(logRangeFold, log::Flow)
      .kv("mode", StringRef(mode))
      .kv("icmp", n.icmp)
      .kv("bool", n.boolean)
      .kv("select", n.select)
      .kv("checks", checks)
      .kv("queries", n.queries)
      .kv("facts", n.facts)
      .kv("facts-skipped", n.factsSkipped)
      .kv("facts-used", n.factsUsed)
      .kv("no-facts", n.noFacts)
      .kv("depth-cap", n.depthCap)
      .kv("empty", n.empty)
      .kv("wrap-flags", n.wrapFlags);
  if (folded.empty() && !check)
    markAllAnalysesPreserved();
}

} // namespace

std::unique_ptr<Pass> createRangeFoldPass(RangeFoldMode mode,
                                          unsigned maxDepth) {
  return std::make_unique<RangeFoldPass>(mode, maxDepth);
}

std::unique_ptr<Pass> createRangeFoldPass() {
  return std::make_unique<RangeFoldPass>();
}

} // namespace sea
