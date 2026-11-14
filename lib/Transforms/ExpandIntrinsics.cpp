// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExpandIntrinsics.cpp - intrinsic ops into core bit-vector ops -*- C++
//-*-//
//
// Rewrites the intrinsic ops (min/max, abs, bswap, funnel shifts, overflow
// bits) into core bit-vector ops the VC encodes. abs's nsw (LLVM's
// is_int_min_poison) stays a promise: it becomes the nsw of the expansion's
// `0 - x`, which overflows exactly at the minimal signed value. The
// interpreter executes both forms, which is what lets the oracles validate
// this pass.
//
// Contract.
//   Accepts: any program.
//   Produces: no intrinsic op (checked), except sea.umulo / sea.smulo with
//     keep-mul-overflow. Each expansion computes the op's value (check-
//     semantics --sea-opt); the op computing the result is created last
//     and all created ops take the intrinsic's location (oracles rely on
//     it: the last value traced at the location is the result).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

class ExpandIntrinsicsPass
    : public PassWrapper<ExpandIntrinsicsPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ExpandIntrinsicsPass)

  ExpandIntrinsicsPass() = default;
  ExpandIntrinsicsPass(const ExpandIntrinsicsPass &other)
      : PassWrapper(other) {}
  explicit ExpandIntrinsicsPass(bool keepMul) { keepMulOverflow = keepMul; }

  StringRef getArgument() const final { return "sea-expand-intrinsics"; }
  StringRef getDescription() const final {
    return "Expand intrinsic ops (min/max, abs, bswap, funnel shifts, "
           "overflow bits) into core bit-vector ops";
  }

  void runOnOperation() override;

  Option<bool> keepMulOverflow{
      *this, "keep-mul-overflow",
      llvm::cl::desc("Leave sea.umulo / sea.smulo for a solver that encodes "
                     "them natively"),
      llvm::cl::init(false)};
};

/// Builds core ops at a fixed insertion point and location.
struct Expander {
  OpBuilder &b;
  Location loc;

  Type iN(unsigned w) { return IntegerType::get(b.getContext(), w); }
  Value cst(const APInt &v) {
    Type t = iN(v.getBitWidth());
    return ConstOp::create(b, loc, t, IntegerAttr::get(t, v));
  }
  Value cst(unsigned w, uint64_t v) { return cst(APInt(w, v)); }
  /// 1 iff x is the minimal signed value.
  Value isMinSigned(Value x) {
    return cmp(ICmpPredicate::eq, x,
               cst(APInt::getSignedMinValue(bitWidthOf(x.getType()))));
  }
  Value cmp(ICmpPredicate p, Value x, Value y) {
    return ICmpOp::create(b, loc, p, x, y);
  }
  Value select(Value c, Value x, Value y) {
    return SelectOp::create(b, loc, x.getType(), c, x, y);
  }
  Value extract(Value x, unsigned low, unsigned w) {
    return ExtractOp::create(b, loc, iN(w), x, b.getI64IntegerAttr(low));
  }
  Value concat(Value hi, Value lo) {
    return ConcatOp::create(
        b, loc, iN(bitWidthOf(hi.getType()) + bitWidthOf(lo.getType())), hi,
        lo);
  }
  template <typename Op>
  Value bin(Value x, Value y) {
    return Op::create(b, loc, x.getType(), x, y);
  }
  template <typename Op>
  Value cast(Value x, unsigned w) {
    return Op::create(b, loc, iN(w), x);
  }
  /// 1 iff x is negative.
  Value isNeg(Value x) {
    return cmp(ICmpPredicate::slt, x, cst(bitWidthOf(x.getType()), 0));
  }

  Value minMax(ICmpPredicate p, Value x, Value y) {
    return select(cmp(p, x, y), x, y);
  }

  /// ite(x <s 0, 0 - x, x); nsw on the negation carries abs's promise.
  Value abs(Value x, bool nsw) {
    Value neg =
        SubOp::create(b, loc, x.getType(), cst(bitWidthOf(x.getType()), 0), x,
                      nsw ? b.getUnitAttr() : UnitAttr(), UnitAttr());
    return select(isNeg(x), neg, x);
  }

  /// Bytes in reverse order: byte 0 becomes the most significant.
  Value bswap(Value x) {
    unsigned n = bitWidthOf(x.getType()) / 8;
    Value acc = extract(x, 0, 8);
    for (unsigned i = 1; i < n; ++i)
      acc = concat(acc, extract(x, 8 * i, 8));
    return acc;
  }

  /// The sum of the bits, each zero-extended to the width.
  Value ctpop(Value x) {
    unsigned w = bitWidthOf(x.getType());
    Value acc = cst(w, 0);
    for (unsigned i = 0; i < w; ++i) {
      Value bit = extract(x, i, 1);
      if (w > 1)
        bit = ZExtOp::create(b, loc, x.getType(), bit);
      acc = bin<AddOp>(acc, bit);
    }
    return acc;
  }

  /// fshl: (hi << s) | (lo >> (w - s)), s = shift mod w. For s = 0 the
  /// second shift is by w, which is 0 in SMT-LIB semantics, giving hi.
  /// fshr: (lo >> s) | (hi << (w - s)).
  Value funnel(bool left, Value hi, Value lo, Value shift) {
    unsigned w = bitWidthOf(hi.getType());
    Value width = cst(w, w); // w < 2^w for every w >= 1
    Value s = bin<URemOp>(shift, width);
    Value rest = bin<SubOp>(width, s);
    if (left)
      return bin<OrOp>(bin<ShlOp>(hi, s), bin<LShrOp>(lo, rest));
    return bin<OrOp>(bin<LShrOp>(lo, s), bin<ShlOp>(hi, rest));
  }

  /// Signed add overflows iff both operands have the sign opposite to the
  /// result's: ((r ^ x) & (r ^ y)) < 0.
  Value saddOvf(Value x, Value y) {
    Value r = bin<AddOp>(x, y);
    return isNeg(bin<AndOp>(bin<XorOp>(r, x), bin<XorOp>(r, y)));
  }
  /// Signed sub overflows iff the operands' signs differ and the result's
  /// sign differs from x's: ((x ^ y) & (x ^ r)) < 0.
  Value ssubOvf(Value x, Value y) {
    Value r = bin<SubOp>(x, y);
    return isNeg(bin<AndOp>(bin<XorOp>(x, y), bin<XorOp>(x, r)));
  }
  /// The product in 2w bits does not fit in w bits: unsigned, the high half
  /// is not 0; signed, it is not the sign extension of the low half.
  Value mulOvf(bool isSigned, Value x, Value y) {
    unsigned w = bitWidthOf(x.getType());
    Value p = isSigned
                  ? bin<MulOp>(cast<SExtOp>(x, 2 * w), cast<SExtOp>(y, 2 * w))
                  : bin<MulOp>(cast<ZExtOp>(x, 2 * w), cast<ZExtOp>(y, 2 * w));
    if (!isSigned)
      return cmp(ICmpPredicate::ne, extract(p, w, w), cst(w, 0));
    return cmp(ICmpPredicate::ne, cast<SExtOp>(extract(p, 0, w), 2 * w), p);
  }
};

void ExpandIntrinsicsPass::runOnOperation() {
  OpBuilder b(&getContext());
  SmallVector<Operation *> expanded;
  getOperation().walk([&](Operation *op) {
    b.setInsertionPoint(op);
    Expander e{b, op->getLoc()};
    auto in = [&](unsigned i) { return op->getOperand(i); };
    Value r =
        llvm::TypeSwitch<Operation *, Value>(op)
            .Case([&](UMinOp) {
              return e.minMax(ICmpPredicate::ult, in(0), in(1));
            })
            .Case([&](UMaxOp) {
              return e.minMax(ICmpPredicate::ugt, in(0), in(1));
            })
            .Case([&](SMinOp) {
              return e.minMax(ICmpPredicate::slt, in(0), in(1));
            })
            .Case([&](SMaxOp) {
              return e.minMax(ICmpPredicate::sgt, in(0), in(1));
            })
            .Case([&](AbsOp a) { return e.abs(in(0), a.getNsw()); })
            .Case([&](BSwapOp) { return e.bswap(in(0)); })
            .Case([&](CtPopOp) { return e.ctpop(in(0)); })
            .Case([&](FShlOp) { return e.funnel(true, in(0), in(1), in(2)); })
            .Case([&](FShrOp) { return e.funnel(false, in(0), in(1), in(2)); })
            .Case([&](UAddOOp) {
              // The sum wraps iff it is below an operand.
              return e.cmp(ICmpPredicate::ult, e.bin<AddOp>(in(0), in(1)),
                           in(0));
            })
            .Case([&](USubOOp) {
              return e.cmp(ICmpPredicate::ult, in(0), in(1));
            })
            .Case([&](SAddOOp) { return e.saddOvf(in(0), in(1)); })
            .Case([&](SSubOOp) { return e.ssubOvf(in(0), in(1)); })
            .Case([&](UMulOOp) {
              return keepMulOverflow ? Value() : e.mulOvf(false, in(0), in(1));
            })
            .Case([&](SMulOOp) {
              return keepMulOverflow ? Value() : e.mulOvf(true, in(0), in(1));
            })
            .Case([&](NegOOp) { return e.isMinSigned(in(0)); })
            .Case([&](SDivOOp) {
              // Only INT_MIN / -1 overflows.
              unsigned w = bitWidthOf(in(0).getType());
              return e.bin<AndOp>(
                  e.isMinSigned(in(0)),
                  e.cmp(ICmpPredicate::eq, in(1), e.cst(APInt::getAllOnes(w))));
            })
            .Default([](Operation *) { return Value(); });
    if (!r)
      return;
    op->getResult(0).replaceAllUsesWith(r);
    expanded.push_back(op);
  });
  for (Operation *op : expanded)
    op->erase();
  getOperation().walk([&](Operation *op) {
    bool keptMul = keepMulOverflow && isa<UMulOOp, SMulOOp>(op);
    if (!keptMul && isa<UMinOp, UMaxOp, SMinOp, SMaxOp, AbsOp, BSwapOp, CtPopOp,
                        FShlOp, FShrOp, UAddOOp, SAddOOp, USubOOp, SSubOOp,
                        UMulOOp, SMulOOp, SDivOOp, NegOOp>(op)) {
      (void)postconditionFailed("sea-expand-intrinsics", "an intrinsic op", op);
      signalPassFailure();
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
}

} // namespace

std::unique_ptr<Pass> createExpandIntrinsicsPass(bool keepMulOverflow) {
  return std::make_unique<ExpandIntrinsicsPass>(keepMulOverflow);
}

} // namespace sea
