// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LowerBoxAll.cpp - sea.box.all -> one arbitrary index ---------------===//
//
// sea-lower-box-all (docs/design/BoxAll.md): a sea.box.all whose result is
// only asserted is replaced by its body at one arbitrary index of the box
// (Skolemization of a universal claim in a positive position).
//
// Assert polarity. Every use of the result is a sea.assert, or an operand
// of an i1 sea.and / sea.or whose result is, recursively, in assert
// polarity. These connectives are monotone, so the program's assertion is
// a monotone function F of the claim's value A = AND_k P(k). With B(t) the
// body at the index of t (or 1 outside the box), A <= B(t) for every t,
// and A = B(t) for some t (a false index if there is one). So F(A) fails
// iff F(B(t)) fails for some t: the lowered program's assertion fails on
// some input iff the original's does (and for each op independently, so
// for several ops in one assertion too). Anything else (assume, a branch,
// xor / icmp / select / any other value use, a strong print) is refused.
// The rule follows the uses of the claim only: `or(not ok, all)` is in
// assert polarity whatever `ok` is.
//
// The replacement, per dimension d (lo_d, step_d constants, ub_d SSA):
//   N_d  = the trip count: 0 if ub_d <=u lo_d, else
//          (ub_d - lo_d - 1) /u step_d + 1 (Semantics.h boxTripCount)
//   t_d  = sea.nondet "box.all.<tag>.<d>" [fresh site] : i64
//   sea.assume (sea.icmp ult, t_d, N_d)
//   k_d  = lo_d + step_d * t_d          (no wrap: k_d < ub_d)
// and the body cloned with k_d for its arguments; its yield is the value.
// <tag> is the op's NameLoc name, else b<n> (its position among the
// program's box ops). The assume never blocks a path the original takes:
//   - every N_d a constant >= 1 (ub_d folds): the body at the index,
//     straight-line, at the op's place;
//   - some N_d constant 0: the box is empty; the value is 1, no index;
//   - otherwise (the box may be empty): the op's block is split at the op;
//     `sea.cond_br (and_d ub_d >u lo_d), ^box, ^join(1)` with the nondets,
//     assumes and body in ^box, which ends `sea.br ^join(v)`; the claim is
//     ^join's argument. An unguarded body would read memory at an index
//     outside the box when the box is empty (a load the original never
//     makes), and `assume(t ult 0)` would block the path and its other
//     asserts. Block ids (kBlockIdsAttr), as sea-guard-null-memhavoc.
// A box.all without uses is erased (dead; the body is pure).
//
// Expansion (option expand-max = K <= kMaxBoxAllExpand, sea-vc
// --box-all-expand; untuned default kBoxAllExpandDefault). A box whose
// upper bounds all fold to constants (foldConstant: the op folders reduce
// them to one, i.e. sea.const and flag-free arithmetic on constants; the
// pass runs before sccp, so a block argument or nsw / nuw arithmetic is
// not a constant here) and whose trip count (the product over its dimensions)
// is at most K becomes the body at every index, row-major, and-reduced: exact
// anywhere, but taken under the same polarity rule and refusals, so what a
// front end may write does not depend on K. Nothing else expands (no
// symbolic bound, no partial unrolling).
//
// Contract.
//   Accepts: any sea.program; its box.all ops verified (docs/design/BoxAll.md).
//   Produces: no sea.box.all. Checked (sea-lower-box-all.postcondition).
//   Errors: a box.all whose result is not in assert polarity, one error
//     per op, kind box-all.<reason>: use-assume, use-branch, use-negation
//     (xor, icmp of the claim), use-select, use-print, use-value. Nothing is
//     lowered then (no partial lowering, no unrolling fallback), expandable
//     boxes included.
//   Reports: the counters (kBoxAllStatsAttr on the program: lowered (one
//     index), guarded, expanded, expand-max, empty, dead, nondets; the
//     lower.box-all log, refusals by reason included).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/Sea/Semantics.h"
#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"

using namespace mlir;

namespace sea {

static log::Tag logBoxAll("lower.box-all", 2,
                          "sea-lower-box-all: the counters of a run (1), "
                          "each op (2)");

namespace {

class LowerBoxAllPass
    : public PassWrapper<LowerBoxAllPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerBoxAllPass)

  LowerBoxAllPass() = default;
  LowerBoxAllPass(const LowerBoxAllPass &other) : PassWrapper(other) {}
  explicit LowerBoxAllPass(unsigned k) { expandMax = k; }

  StringRef getArgument() const final { return "sea-lower-box-all"; }
  StringRef getDescription() const final {
    return "Replace a sea.box.all in assert polarity by its body at one "
           "arbitrary index of the box, or, for a constant box of at most "
           "expand-max indices, by the conjunction of the body at every "
           "index (docs/design/BoxAll.md)";
  }

  void runOnOperation() override;

  Option<unsigned> expandMax{
      *this, "expand-max",
      llvm::cl::desc("A box whose upper bounds are constants and whose trip "
                     "count (the product over the dimensions) is at most "
                     "this is expanded: the body at every index, and-reduced "
                     "(untuned default: kBoxAllExpandDefault; 0: never)"),
      llvm::cl::init(kBoxAllExpandDefault)};
};

/// The first use of `claim` not in assert polarity: the reason (empty: all
/// are) and the user.
std::pair<StringRef, Operation *> polarityRefusal(Value claim) {
  SmallVector<Value> work{claim};
  DenseSet<Value> seen{claim};
  while (!work.empty()) {
    Value v = work.pop_back_val();
    for (OpOperand &use : v.getUses()) {
      Operation *user = use.getOwner();
      if (isa<AssertOp>(user))
        continue;
      if (isa<AndOp, OrOp>(user)) {
        if (seen.insert(user->getResult(0)).second)
          work.push_back(user->getResult(0));
        continue;
      }
      StringRef why = isa<AssumeOp>(user)         ? "use-assume"
                      : isa<BrOp, CondBrOp>(user) ? "use-branch"
                      : isa<XorOp, ICmpOp>(user)  ? "use-negation"
                      : isa<SelectOp>(user)       ? "use-select"
                      : isPrintOp(user)           ? "use-print"
                                                  : "use-value";
      return {why, user};
    }
  }
  return {"", nullptr};
}

} // namespace

void LowerBoxAllPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  if (expandMax > kMaxBoxAllExpand) {
    diag::error("box-all.expand-limit",
                "sea-lower-box-all: expand-max " + Twine(expandMax) +
                    " is above the limit (" + Twine(kMaxBoxAllExpand) + ")",
                program.getLoc());
    return signalPassFailure();
  }
  SmallVector<BoxAllOp> boxes;
  program.walk([&](BoxAllOp op) { boxes.push_back(op); });
  if (boxes.empty()) {
    markAllAnalysesPreserved();
    return;
  }

  // Refusals first: all or nothing.
  llvm::StringMap<uint64_t> refused;
  for (BoxAllOp box : boxes) {
    auto [why, user] = polarityRefusal(box.getResult());
    if (why.empty())
      continue;
    ++refused[why];
    diag::error(("box-all." + why).str(),
                "sea.box.all: its result is used by '" +
                    user->getName().getStringRef() +
                    "', not only asserted through i1 sea.and / sea.or "
                    "(the lowering is exact only in assert polarity; "
                    "docs/design/BoxAll.md)",
                box.getLoc());
  }
  if (!refused.empty()) {
    auto count = [&](StringRef why) { return refused.lookup(why); };
    SEA_LOG(logBoxAll, log::Flow)
        .kv("lowered", uint64_t(0))
        .kv("refused-use-assume", count("use-assume"))
        .kv("refused-use-branch", count("use-branch"))
        .kv("refused-use-negation", count("use-negation"))
        .kv("refused-use-select", count("use-select"))
        .kv("refused-use-print", count("use-print"))
        .kv("refused-use-value", count("use-value"));
    return signalPassFailure();
  }

  // fresh nondet sites, after every site of the program
  uint64_t nextSite = 0;
  program.walk([&](Operation *op) {
    if (auto s = op->getAttrOfType<IntegerAttr>("site"))
      nextSite = std::max<uint64_t>(nextSite, s.getValue().getZExtValue() + 1);
  });

  auto i64 = IntegerType::get(ctx, 64);
  auto i1 = IntegerType::get(ctx, 1);
  OpBuilder b(ctx);
  BlockIdNamer ids(program);
  uint64_t lowered = 0, guarded = 0, empty = 0, dead = 0, nondets = 0,
           expanded = 0;
  for (auto [ordinal, box] : llvm::enumerate(boxes)) {
    Location loc = box.getLoc();
    if (box->use_empty()) {
      ++dead;
      SEA_LOG(logBoxAll, log::Object).msg("dead").at(loc);
      box.erase();
      continue;
    }
    const unsigned n = box.getNumDims();
    b.setInsertionPoint(box);
    auto cst = [&](Type t, const APInt &v) -> Value {
      return ConstOp::create(b, loc, t, IntegerAttr::get(t, v));
    };
    auto c64 = [&](uint64_t v) { return cst(i64, APInt(64, v)); };
    SmallVector<APInt, 2> lo, step;
    SmallVector<std::optional<APInt>, 2> constTrips;
    bool knownEmpty = false;
    for (unsigned d = 0; d < n; ++d) {
      lo.push_back(APInt(64, uint64_t(box.getLowerBounds()[d])));
      step.push_back(APInt(64, uint64_t(box.getSteps()[d])));
      std::optional<APInt> ub = foldConstant(box.getUpperBounds()[d]);
      constTrips.push_back(
          ub ? std::optional<APInt>(boxTripCount(lo[d], step[d], *ub))
             : std::nullopt);
      knownEmpty |= constTrips.back() && constTrips.back()->isZero();
    }
    if (knownEmpty) {
      ++empty;
      SEA_LOG(logBoxAll, log::Object).msg("empty").at(loc);
      box.getResult().replaceAllUsesWith(cst(i1, APInt(1, 1)));
      box.erase();
      continue;
    }
    // a constant box of at most expand-max indices: the body at every
    // index, row-major, and-reduced (the same polarity rule as above)
    uint64_t total = 1;
    bool allConst = true;
    for (unsigned d = 0; d < n && allConst; ++d) {
      if (!constTrips[d]) {
        allConst = false;
        break;
      }
      // over expand-max (or past 64 bits): not expanded
      if (constTrips[d]->ugt(expandMax) ||
          (total *= constTrips[d]->getZExtValue()) > expandMax)
        allConst = false;
    }
    if (allConst && total <= expandMax) {
      Block &body = box.getBodyBlock();
      Value all;
      SmallVector<uint64_t, 2> t(n, 0);
      for (uint64_t visited = 0; visited < total; ++visited) {
        IRMapping map;
        for (unsigned d = 0; d < n; ++d)
          map.map(body.getArgument(d),
                  c64((lo[d] + step[d] * t[d]).getZExtValue()));
        for (Operation &op : body.without_terminator())
          b.clone(op, map);
        Value v = map.lookupOrDefault(box.getYielded());
        all = all ? Value(AndOp::create(b, loc, all, v)) : v;
        for (unsigned d = n; d-- > 0;) {
          if (++t[d] != constTrips[d]->getZExtValue())
            break;
          t[d] = 0;
        }
      }
      ++expanded;
      SEA_LOG(logBoxAll, log::Object)
          .msg("expanded")
          .kv("indices", total)
          .at(loc);
      box.getResult().replaceAllUsesWith(all);
      box.erase();
      continue;
    }
    // the trip counts (valid where the box is not empty) and the conditions
    // of a non-empty box for the dimensions whose bound is not a constant
    SmallVector<Value, 2> trips;
    Value nonEmpty;
    for (unsigned d = 0; d < n; ++d) {
      if (constTrips[d]) {
        trips.push_back(c64(constTrips[d]->getZExtValue()));
        continue;
      }
      Value ub = box.getUpperBounds()[d];
      Value loConst = c64(lo[d].getZExtValue());
      Value c = ICmpOp::create(b, loc, ICmpPredicate::ugt, ub, loConst);
      nonEmpty = nonEmpty ? Value(AndOp::create(b, loc, nonEmpty, c)) : c;
      Value span =
          lo[d].isZero() ? ub : Value(SubOp::create(b, loc, ub, loConst));
      if (!step[d].isOne()) {
        Value s1 = SubOp::create(b, loc, span, c64(1));
        Value q = UDivOp::create(b, loc, s1, c64(step[d].getZExtValue()));
        span = AddOp::create(b, loc, q, c64(1));
      }
      trips.push_back(span);
    }
    // the guarded form: the box in a block of its own
    Block *join = nullptr;
    if (nonEmpty) {
      Block *blk = box->getBlock();
      join = blk->splitBlock(box->getNextNode());
      BlockArgument claim = join->addArgument(i1, loc);
      box.getResult().replaceAllUsesWith(claim);
      Block *inBox = new Block();
      program.getBody().getBlocks().insertAfter(Region::iterator(blk), inBox);
      OpBuilder hb(ctx);
      hb.setInsertionPointToEnd(blk);
      Value one = ConstOp::create(hb, loc, i1, IntegerAttr::get(i1, 1));
      auto cbr = CondBrOp::create(hb, loc, nonEmpty, ValueRange{},
                                  ValueRange{one}, inBox, join);
      if (ArrayAttr l = ids.splitTail(join))
        cbr->setAttr(kBlockIdsAttr, l);
      b.setInsertionPointToStart(inBox);
      ++guarded;
    }
    // the tag of the nondets: the op's name, else its position
    std::string tag;
    if (auto nl = dyn_cast<NameLoc>(loc))
      tag = nl.getName().str();
    else
      tag = "b" + std::to_string(ordinal);
    IRMapping map;
    Block &body = box.getBodyBlock();
    for (unsigned d = 0; d < n; ++d) {
      std::string name = "box.all." + tag + "." + std::to_string(d);
      Location ndLoc = NameLoc::get(StringAttr::get(ctx, name), loc);
      Value t = NondetOp::create(b, ndLoc, i64, b.getStringAttr(name),
                                 b.getI64IntegerAttr(nextSite++));
      ++nondets;
      AssumeOp::create(b, loc,
                       ICmpOp::create(b, loc, ICmpPredicate::ult, t, trips[d]));
      Value k = t;
      if (!step[d].isOne())
        k = MulOp::create(b, loc, k, c64(step[d].getZExtValue()));
      if (!lo[d].isZero())
        k = AddOp::create(b, loc, k, c64(lo[d].getZExtValue()));
      map.map(body.getArgument(d), k);
    }
    for (Operation &op : body.without_terminator())
      b.clone(op, map);
    Value v = map.lookupOrDefault(box.getYielded());
    if (join) {
      BlockIdNamer::set(BrOp::create(b, loc, ValueRange{v}, join),
                        ids.derive(join, "x"));
    } else {
      box.getResult().replaceAllUsesWith(v);
    }
    SEA_LOG(logBoxAll, log::Object)
        .msg("lowered")
        .kv("dims", uint64_t(n))
        .kv("guarded", join ? 1 : 0)
        .at(loc);
    box.erase();
    ++lowered;
  }

  // postcondition: no box left
  WalkResult left = program.walk([&](BoxAllOp op) {
    (void)postconditionFailed("sea-lower-box-all", "a sea.box.all", op);
    return WalkResult::interrupt();
  });
  if (left.wasInterrupted())
    return signalPassFailure();

  SmallVector<NamedAttribute> stats;
  auto stat = [&](StringRef key, uint64_t v) {
    stats.push_back(
        NamedAttribute(StringAttr::get(ctx, key), IntegerAttr::get(i64, v)));
  };
  stat("lowered", lowered);
  stat("guarded", guarded);
  stat("empty", empty);
  stat("dead", dead);
  stat("nondets", nondets);
  stat("expanded", expanded);
  stat("expand-max", expandMax);
  program->setAttr(kBoxAllStatsAttr, DictionaryAttr::get(ctx, stats));
  SEA_LOG(logBoxAll, log::Flow)
      .kv("lowered", lowered)
      .kv("guarded", guarded)
      .kv("empty", empty)
      .kv("dead", dead)
      .kv("nondets", nondets)
      .kv("expanded", expanded)
      .kv("expand-max", uint64_t(expandMax));
}

std::unique_ptr<Pass> createLowerBoxAllPass() {
  return std::make_unique<LowerBoxAllPass>();
}

std::unique_ptr<Pass> createLowerBoxAllPass(unsigned expandMax) {
  return std::make_unique<LowerBoxAllPass>(expandMax);
}

} // namespace sea
