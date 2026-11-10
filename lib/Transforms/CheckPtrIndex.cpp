// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- CheckPtrIndex.cpp - the GEP-promise discharge job's instrumentation ===//
//
// docs/design/PtrIndex.md section 6 (paranoid mode). The no-wrap flags of a
// sea.ptr.index (inbounds / nusw / nuw) are trusted front-end promises.
// This pass validates them in paranoid mode because they can be checked.
// The stride rule uses a promise only at an access whose
// address is the GEP (sea-vc --ack-stride, --seam-stride). A broken promise
// is poison in LLVM, UB only where it is used, and that is where this job
// checks it. Before every load / store / memset / memmove (destination and
// source) whose address comes from flagged GEPs, it asserts each GEP's
// promise:
//     P1  every x_k * s_k fits n bits,
//     P2  O = sum x_k * s_k + c fits n bits,
//     P3  base + O is in [0, 2^n),
// signed for inbounds / nusw (x_k sign-extended), unsigned for nuw (the
// sign-extended index read unsigned); both when both flags are set. The
// arithmetic is exact in w = 2n + 8 + ceil(log2(m + 2)) bits, as
// Semantics.cpp brokenPtrIndexPromise and op::ptr::promise compute it.
//
// An address "comes from" a GEP through a chain (PtrIndex.md):
//   - the GEP itself, and the base of a GEP (a nested GEP's promise is a
//     fact at the access too: poison through a base is poison);
//   - the base of a ptr.add / addr.add (the bytes sea-split-wide-access
//     makes, a field after a GEP);
//   - sea.select, the promise guarded by the condition.
// A block argument (a phi) is not followed: the GEPs that reach an access
// only through one get no assertion there, and the job says so (warning
// check-ptr-index.phi).
//
// The program's own asserts are dropped (the job checks the promises on
// every execution, not the functional property); sea-merge-asserts then
// makes the BMC form. Run after placement (addresses are integers), before
// sea-split-wide-access (which makes the byte addresses: followed anyway).
//
// Contract.
//   Accepts: a placed program.
//   Produces: one assert per (access, promised GEP) pair, no original
//     assert.
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;

namespace sea {
namespace {

class CheckPtrIndexPass
    : public PassWrapper<CheckPtrIndexPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckPtrIndexPass)

  StringRef getArgument() const final { return "sea-check-ptr-index"; }
  StringRef getDescription() const final {
    return "Assert the no-wrap promise of every flagged sea.ptr.index at the "
           "accesses that use it (the paranoid job of the trusted GEP flags); "
           "drop the program's asserts";
  }

  void runOnOperation() override;

  /// Diagnostic only, never a discharge: the program's asserts become
  /// assumes (only executions that pass every assertion are checked).
  /// Circular with the main job if used as one: an execution that breaks a
  /// promise and an assertion would be missed by both.
  Option<bool> assumeAsserts{*this, "assume-asserts",
                             llvm::cl::desc("diagnostic: assume the "
                                            "program's asserts"),
                             llvm::cl::init(false)};
  CheckPtrIndexPass() = default;
  CheckPtrIndexPass(const CheckPtrIndexPass &o) : PassWrapper(o) {}
};

/// A flagged GEP an address comes from, and the condition (null: true)
/// under which it does.
struct Source {
  PtrIndexOp gep;
  Value guard;
};

void CheckPtrIndexPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  OpBuilder b(ctx);
  SmallVector<AssertOp> original;
  program.walk([&](AssertOp a) { original.push_back(a); });
  bool phi = false;

  // the flagged GEPs \p v comes from, with their guards (built before \p at)
  std::function<void(Value, Value, Location, SmallVectorImpl<Source> &)>
      sources = [&](Value v, Value guard, Location loc,
                    SmallVectorImpl<Source> &out) {
        Operation *def = v.getDefiningOp();
        if (!def) {
          phi = true;
          return;
        }
        // a chain of GEPs and pointer additions: every flagged GEP
        // on it is a fact at the access (poison through a base is poison)
        if (auto g = dyn_cast<PtrIndexOp>(def)) {
          if (g.getInbounds() || g.getNusw() || g.getNuw())
            out.push_back({g, guard});
          sources(g.getBase(), guard, loc, out);
          return;
        }
        if (isa<PtrAddOp, AddrAddOp>(def)) {
          sources(def->getOperand(0), guard, loc, out);
          return;
        }
        if (auto s = dyn_cast<SelectOp>(def)) {
          Value c = s.getCondition();
          Value one = ConstOp::create(b, loc, b.getI1Type(),
                                      IntegerAttr::get(b.getI1Type(), 1));
          Value nc = XorOp::create(b, loc, c, one);
          Value gt = guard ? Value(AndOp::create(b, loc, guard, c)) : c;
          Value gf = guard ? Value(AndOp::create(b, loc, guard, nc)) : nc;
          sources(s.getTrueValue(), gt, loc, out);
          sources(s.getFalseValue(), gf, loc, out);
        }
      };

  // the promise of \p g as an i1 (P1-P3; signed and / or unsigned)
  auto promise = [&](PtrIndexOp g, Location loc) -> Value {
    Value base = g.getBase();
    unsigned n = bitWidthOf(base.getType());
    size_t m = g.getIndices().size();
    unsigned lg = 0;
    while ((size_t(1) << lg) < m + 2)
      ++lg;
    unsigned w = 2 * n + 8 + lg;
    auto iN = IntegerType::get(ctx, n), iW = IntegerType::get(ctx, w);
    auto cst = [&](const APInt &a) -> Value {
      return ConstOp::create(b, loc, iW, IntegerAttr::get(iW, a));
    };
    Value bi = isa<PtrType>(base.getType())
                   ? Value(PtrToIntOp::create(b, loc, iN, base))
                   : Value(AddrToIntOp::create(b, loc, iN, base));
    Value bw = ZExtOp::create(b, loc, iW, bi);
    auto form = [&](bool sgn) -> Value {
      // n-bit two's complement constants of the op, read signed / unsigned
      auto konst = [&](int64_t v) {
        APInt a(64, uint64_t(v), /*isSigned=*/true);
        APInt an = n < 64 ? a.trunc(n) : a;
        return cst(sgn ? an.sext(w) : an.zext(w));
      };
      APInt lo = sgn ? APInt::getSignedMinValue(n).sext(w) : APInt(w, 0);
      APInt hi = sgn ? APInt::getSignedMaxValue(n).sext(w)
                     : APInt::getMaxValue(n).zext(w);
      auto fits = [&](Value t, const APInt &l, const APInt &h) -> Value {
        Value a = ICmpOp::create(b, loc, ICmpPredicate::sle, cst(l), t);
        Value z = ICmpOp::create(b, loc, ICmpPredicate::sle, t, cst(h));
        return AndOp::create(b, loc, a, z);
      };
      SmallVector<Value> conj;
      Value total = konst(g.getOffset());
      for (auto [k, i] : llvm::enumerate(g.getIndices())) {
        Value x = i;
        if (bitWidthOf(i.getType()) < n)
          x = SExtOp::create(b, loc, iN, i);
        Value xw = sgn ? Value(SExtOp::create(b, loc, iW, x))
                       : Value(ZExtOp::create(b, loc, iW, x));
        Value prod = MulOp::create(b, loc, xw, konst(g.getStrides()[k]));
        conj.push_back(fits(prod, lo, hi)); // P1
        total = AddOp::create(b, loc, total, prod);
      }
      conj.push_back(fits(total, lo, hi)); // P2
      Value addr = AddOp::create(b, loc, bw, total);
      conj.push_back(fits(addr, APInt(w, 0),
                          APInt::getMaxValue(n).zext(w))); // P3
      Value r = conj.front();
      for (Value c : ArrayRef<Value>(conj).drop_front())
        r = AndOp::create(b, loc, r, c);
      return r;
    };
    Value r;
    if (g.getInbounds() || g.getNusw())
      r = form(true);
    if (g.getNuw()) {
      Value u = form(false);
      r = r ? Value(AndOp::create(b, loc, r, u)) : u;
    }
    return r;
  };

  auto check = [&](Operation *op, Value addr) {
    Location loc = op->getLoc();
    b.setInsertionPoint(op);
    SmallVector<Source> srcs;
    sources(addr, Value(), loc, srcs);
    for (const Source &s : srcs) {
      Value ok = promise(s.gep, loc);
      if (s.guard) {
        Value one = ConstOp::create(b, loc, b.getI1Type(),
                                    IntegerAttr::get(b.getI1Type(), 1));
        Value ng = XorOp::create(b, loc, s.guard, one);
        ok = OrOp::create(b, loc, ng, ok);
      }
      AssertOp::create(b, loc, ok);
    }
  };
  program.walk([&](Operation *op) {
    if (auto ld = dyn_cast<LoadOp>(op))
      check(op, ld.getAddr());
    else if (auto st = dyn_cast<StoreOp>(op))
      check(op, st.getAddr());
    else if (auto ms = dyn_cast<MemSetOp>(op))
      check(op, ms.getAddr());
    else if (auto mv = dyn_cast<MemMoveOp>(op)) {
      check(op, mv.getAddr());
      check(op, mv.getSrcAddr());
    }
  });
  for (AssertOp a : original) {
    b.setInsertionPoint(a);
    // the top of a tagged assert stays a reach target (sea-vc --reach)
    if (Attribute site = a->getAttr(kReachSiteAttr))
      createReachSiteMarker(b, a.getLoc(), a->getName().getStringRef(), site);
    if (assumeAsserts)
      AssumeOp::create(b, a.getLoc(), a.getOperand());
    a.erase();
  }
  if (phi)
    diag::warn("check-ptr-index.phi",
               "an access address is a block argument (a phi): the promises "
               "of the GEPs that reach it are not checked there");
}

} // namespace

std::unique_ptr<Pass> createCheckPtrIndexPass(bool assumeAsserts) {
  auto p = std::make_unique<CheckPtrIndexPass>();
  p->assumeAsserts = assumeAsserts;
  return p;
}

} // namespace sea
