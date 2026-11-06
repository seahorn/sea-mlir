// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Replay.cpp - replay a solver model in the interpreter ----*- C++ -*-===//

#include "sea/VC/Replay.h"

#include "sea/Expr/ExprAPInt.hh"

#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "llvm/ADT/DenseSet.h"

#include <map>
#include <memory>

using namespace mlir;
using namespace expr;
using namespace expr::op;

namespace sea {

std::optional<llvm::APInt> numeralToAPInt(Expr num, unsigned width) {
  unsigned w;
  if (!num || !bv::isBvNum(num, w) || w != width)
    return std::nullopt;
  // A backend may return a signed numeral: normalize into [0, 2^w).
  mpz_class n = bv::toMpz(num);
  mpz_class mod = mpz_class(1UL) << width;
  n = ((n % mod) + mod) % mod;
  return toAPInt(width, n);
}

FailureOr<llvm::APInt> ModelSource::get(NondetOp op, std::string &origin,
                                        std::string &error) {
  unsigned width = op.getType().getIntOrFloatBitWidth();
  Expr t = sem.solverTerm(sem.lookup(op.getResult(), ctx));
  Expr v = t ? model.eval(t, /*complete=*/false) : Expr();
  std::optional<llvm::APInt> val = numeralToAPInt(v, width);
  if (!val) {
    // the model does not constrain it (the evaluation returned a term, not a
    // value): any value is consistent with the model
    if (v && !isOpX<NONDET>(v)) { // NONDET: the evaluation itself failed
      origin = "default";
      return llvm::APInt(width, 0);
    }
    error =
        "model has no numeral for nondet site " + std::to_string(op.getSite());
    return failure();
  }
  origin = "model";
  return *val;
}

FailureOr<llvm::APInt>
ModelSource::getCell(Operation *siteOp, const llvm::APInt &addr,
                     StringRef object, unsigned cellWidth, std::string &origin,
                     std::string &error) {
  if (!object.empty()) {
    error = "the VC has no model for abstract memories (place first)";
    return failure();
  }
  // The array whose cells the site provides: the region's initial memory,
  // or the fresh array of a memhavoc.
  Expr arr;
  uint64_t site;
  if (auto mn = dyn_cast<MemNondetOp>(siteOp)) {
    arr = sem.lambdaMemory() ? sem.initialArray(mn)
                             : sem.solverTerm(sem.lookup(mn.getResult(), ctx));
    site = mn.getSite();
  } else {
    auto mh = cast<MemHavocOp>(siteOp);
    // the solver's term: with SeaM the array's index sort is ptr(w), erased
    // to bv(w) (PtrSort S2e)
    arr = sem.havocArray(mh);
    if (arr && !sem.lambdaMemory())
      arr = sem.solverTerm(arr);
    site = mh.getSite();
  }
  if (!arr) {
    error = "no VC array for memory site " + std::to_string(site);
    return failure();
  }
  Expr idx = sem.numeral(addr);
  Expr cell = op::array::select(arr, idx);
  Expr cv = model.eval(cell, /*complete=*/false);
  std::optional<llvm::APInt> val = numeralToAPInt(cv, cellWidth);
  if (!val) {
    // unconstrained by the model (a term, not a value): any value
    if (cv && !isOpX<NONDET>(cv)) { // NONDET: the evaluation itself failed
      origin = "default";
      return llvm::APInt(cellWidth, 0);
    }
    error = "model has no numeral for a cell of memory site " +
            std::to_string(site);
    return failure();
  }
  // a numeral may still be a default: a cell no constraint reads (a
  // solver's array `else`, the 0 of a lowering's function); the value is
  // kept, so validation compares the same value
  origin = model.defaultCell(arr, idx) ? "default" : "model";
  return *val;
}

std::optional<llvm::APInt> ModelSource::peekCell(Operation *siteOp,
                                                 const llvm::APInt &addr,
                                                 StringRef object,
                                                 unsigned cellWidth,
                                                 std::string &origin) {
  std::string error;
  FailureOr<llvm::APInt> v =
      getCell(siteOp, addr, object, cellWidth, origin, error);
  if (failed(v))
    return std::nullopt;
  return *v;
}

RunResult replayPrints(ProgramOp program, solver::Model &model, SeaOpSem &sem,
                       OpSemContext &ctx, const RunOptions &options) {
  ModelSource source(model, sem, ctx);
  RunOptions o = options;
  o.warnEarlierVersion = false; // the validation run counts them
  return interpret(program, source, o);
}

namespace {
/// The concrete pointer companion of a replayed run (PtrIntObservation):
/// address -> bits of the pointer last stored there, one per memory chain
/// (the interpreter runs memory in place: an update hands its chain on).
class PtrIntMonitor {
public:
  explicit PtrIntMonitor(ReplayResult &result) : result(result) {}

  void onOp(Operation &op,
            llvm::function_ref<std::optional<llvm::APInt>(Value)> valueOf) {
    if (isa<MemConstOp, MemNondetOp>(op)) {
      shadows[op.getResult(0)] = fresh();
      return;
    }
    if (auto st = dyn_cast<StoreOp>(op)) {
      std::shared_ptr<Shadow> sh = handOn(st.getMem(), st.getResult());
      if (std::optional<Value> p = storedPointer(st))
        if (auto a = valueOf(st.getAddr()))
          if (auto bits = valueOf(*p))
            (*sh)[*a] = *bits;
      return;
    }
    if (auto mv = dyn_cast<MemMoveOp>(op)) {
      // OpSem: mcopy of the companions for 1-unit cells; else kept
      std::shared_ptr<Shadow> src = shadowOf(mv.getSrc());
      Shadow from = *src; // read before the destination changes
      std::shared_ptr<Shadow> dst = handOn(mv.getMem(), mv->getResult(0));
      auto memTy = cast<MemType>(mv.getMem().getType());
      auto d = valueOf(mv.getAddr()), s = valueOf(mv.getSrcAddr()),
           len = valueOf(mv.getLen());
      if (memTy.getCellUnits() != 1 || !d || !s || !len)
        return;
      llvm::APInt n = len->zextOrTrunc(d->getBitWidth());
      if (len->getActiveBits() > d->getBitWidth())
        n = llvm::APInt::getAllOnes(d->getBitWidth());
      for (auto it = dst->begin(); it != dst->end();) {
        auto cur = it++;
        if ((cur->first - *d).ult(n))
          dst->erase(cur);
      }
      for (auto &[k, v] : from)
        if (llvm::APInt off = k - *s; off.ult(n))
          (*dst)[*d + off] = v;
      return;
    }
    if (isa<MemSetOp, MemHavocOp>(op)) {
      handOn(op.getOperand(0), op.getResult(0)); // kept
      return;
    }
    if (auto br = dyn_cast<BrOp>(op)) {
      bind(br.getDest(), br.getDestOperands());
      return;
    }
    if (auto cbr = dyn_cast<CondBrOp>(op)) {
      std::optional<llvm::APInt> c = valueOf(cbr.getCondition());
      if (!c)
        return;
      if (c->isOne())
        bind(cbr.getTrueDest(), cbr.getTrueDestOperands());
      else
        bind(cbr.getFalseDest(), cbr.getFalseDestOperands());
      return;
    }
    auto fb = dyn_cast<AddrFromBitsOp>(op);
    if (!fb)
      return;
    // OpSem companionLoad: from_bits of a concat whose first byte is a load
    // of a pointer-indexed memory
    Value v = fb.getOperand();
    while (auto cc = v.getDefiningOp<ConcatOp>())
      v = cc.getOperand(1);
    auto ld = v.getDefiningOp<LoadOp>();
    if (!ld || v == fb.getOperand() || !pointerIndexed(ld.getMem()))
      return;
    auto a = valueOf(ld.getAddr());
    auto bits = valueOf(fb.getOperand());
    if (!a || !bits)
      return;
    PtrIntObservation o{&op, ld, *a, *bits, std::nullopt};
    Shadow &sh = *shadowOf(ld.getMem());
    if (auto it = sh.find(*a); it != sh.end())
      o.stored = it->second;
    result.ptrInt.push_back(std::move(o));
  }

private:
  struct KeyLess {
    bool operator()(const llvm::APInt &a, const llvm::APInt &b) const {
      return a.getBitWidth() != b.getBitWidth()
                 ? a.getBitWidth() < b.getBitWidth()
                 : a.ult(b);
    }
  };
  using Shadow = std::map<llvm::APInt, llvm::APInt, KeyLess>;
  static std::shared_ptr<Shadow> fresh() { return std::make_shared<Shadow>(); }

  static bool pointerIndexed(Value mem) {
    auto memTy = dyn_cast<MemType>(mem.getType());
    return memTy && isa<AddrType>(memTy.getIndex());
  }
  /// OpSem storedPointer: the address value a store writes (whole, or the
  /// offset-0 byte of a split one)
  static std::optional<Value> storedPointer(StoreOp st) {
    Value v = st.getValue();
    if (isa<AddrType>(v.getType()))
      return v;
    if (auto ex = v.getDefiningOp<ExtractOp>())
      if (ex.getLow() == 0)
        if (auto tb = ex.getOperand().getDefiningOp<AddrToBitsOp>())
          return tb.getOperand();
    return std::nullopt;
  }
  /// a memory with no recorded producer: empty (OpSem: a fresh companion)
  std::shared_ptr<Shadow> shadowOf(Value mem) {
    std::shared_ptr<Shadow> &sh = shadows[mem];
    if (!sh)
      sh = fresh();
    return sh;
  }
  std::shared_ptr<Shadow> handOn(Value from, Value to) {
    std::shared_ptr<Shadow> sh = shadowOf(from);
    shadows[to] = sh;
    return sh;
  }
  void bind(Block *dest, ValueRange operands) {
    for (auto [arg, in] : llvm::zip(dest->getArguments(), operands))
      if (isa<MemType>(in.getType()))
        shadows[arg] = shadowOf(in);
  }

  ReplayResult &result;
  llvm::DenseMap<Value, std::shared_ptr<Shadow>> shadows;
};
} // namespace

ReplayResult replayAndValidate(ProgramOp program, solver::Model &model,
                               SeaOpSem &sem, OpSemContext &ctx) {
  ReplayResult result;
  ModelSource source(model, sem, ctx);
  RunOptions options;
  // prints peek cells through the model: a run of their own (replayPrints),
  // so this run's model evaluations (and their counters) are the same with
  // and without them
  options.prints = false;
  options.warnEarlierVersion = false; // counted: interp.earlier-version-loads
  options.onValue = [&](Value v, const llvm::APInt &interp) {
    ++result.checked;
    Expr t = sem.solverTerm(sem.lookup(v, ctx));
    Expr m = t ? model.eval(t, /*complete=*/false) : Expr();
    std::optional<llvm::APInt> mv = numeralToAPInt(m, interp.getBitWidth());
    // a term rather than a value: the model leaves it unconstrained (the
    // value does not matter): nothing to compare
    if (!mv && m && !isOpX<NONDET>(m)) {
      --result.checked;
      ++result.dontCare;
      return;
    }
    if (!mv || *mv != interp)
      result.mismatches.push_back({v, interp, mv});
    else if (isa_and_present<LoadOp>(v.getDefiningOp()))
      result.matchedLoads.insert(v);
  };
  // the A-PTRINT discharge: its property is the guards, not the asserts
  std::optional<PtrIntMonitor> ptrInt;
  if (sem.ptrIntChecking() && sem.companionChecked()) {
    ptrInt.emplace(result);
    options.onOp = [&](Operation &op, auto valueOf) {
      ptrInt->onOp(op, valueOf);
    };
  }
  result.run = interpret(program, source, options);
  return result;
}

SmallVector<Operation *> noClobberSuspects(const ReplayResult &replay) {
  SmallVector<Operation *> suspects;
  if (replay.mismatches.empty() || replay.run.earlierVersionLoads.empty())
    return suspects;
  llvm::DenseSet<Operation *> earlier(replay.run.earlierVersionLoads.begin(),
                                      replay.run.earlierVersionLoads.end());
  llvm::DenseSet<Operation *> hit;
  // the backward slice of the mismatched values
  llvm::DenseSet<Value> seen;
  SmallVector<Value> work;
  for (const ValueMismatch &m : replay.mismatches) {
    if (Operation *d = m.value.getDefiningOp(); d && earlier.contains(d))
      hit.insert(d); // its own value differs
    if (seen.insert(m.value).second)
      work.push_back(m.value);
  }
  while (!work.empty()) {
    Value v = work.pop_back_val();
    auto push = [&](Value u) {
      if (seen.insert(u).second)
        work.push_back(u);
    };
    if (auto arg = dyn_cast<BlockArgument>(v)) {
      Block *b = arg.getOwner();
      for (auto it = b->pred_begin(), e = b->pred_end(); it != e; ++it) {
        auto br = dyn_cast<BranchOpInterface>((*it)->getTerminator());
        if (!br)
          continue;
        SuccessorOperands so = br.getSuccessorOperands(it.getSuccessorIndex());
        if (Value in = so[arg.getArgNumber()])
          push(in);
      }
      continue;
    }
    Operation *d = v.getDefiningOp();
    if (!d)
      continue;
    if (earlier.contains(d) && !replay.matchedLoads.contains(v))
      hit.insert(d);
    for (Value o : d->getOperands())
      push(o);
  }
  for (Operation *l : replay.run.earlierVersionLoads)
    if (hit.contains(l))
      suspects.push_back(l);
  return suspects;
}

} // namespace sea
