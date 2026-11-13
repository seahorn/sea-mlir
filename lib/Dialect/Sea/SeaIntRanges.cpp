// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaIntRanges.cpp - InferIntRangeInterface of the sea value ops -----===//
//
// The integer ranges (signed and unsigned intervals, ConstantIntRanges) of
// the results of the pure bit-vector ops, from the ranges of their operands:
// the upstream transfer functions (mlir/Interfaces/Utils/
// InferIntRangeCommon.h), checked against sea's semantics. Sea ops are
// total SMT-LIB bit-vector ops (CLAUDE.md "Semantics"), while the upstream
// functions are written for arith, where some inputs are poison or UB. Per
// op (an exhaustive check over all ranges of widths 1-3 against evalPureOp,
// unittests/IntRanges, pins each):
//   add, sub, mul, shl   upstream; a possible wrap gives the full range.
//                        nsw / nuw are used when the op carries them
//                        (binding semantics; the VC encoding wraps, an
//                        over-approximation; sea-range-fold counts the
//                        folds that meet them).
//                        shl by >= width (0 in sea) is an overflow upstream:
//                        full range; under a flag upstream saturates it,
//                        while sea's 0 << k keeps the promise: flags only
//                        when every shift amount is below the width
//   lshr, ashr           upstream; a shift amount >= width gives the full
//                        range (sea: 0 / the sign fill)
//   udiv                 upstream; x / 0 = ~0 in sea (arith: UB): when the
//                        divisor may be 0, ~0 is added
//   urem                 upstream; x % 0 = x in sea: when the divisor may be
//                        0, the dividend's range is added
//   sdiv, srem           upstream; a divisor that may be 0 gives the full
//                        range there already; INT_MIN / -1 (INT_MIN in sea)
//                        is an overflow upstream: full range; srem's
//                        INT_MIN % -1 = 0 is inside its bound
//   and, or, xor, umin, umax, smin, smax, zext, sext, trunc: upstream
//                        (total there too)
//   icmp                 intrange::evaluatePred: 0 or 1 when the ranges
//                        decide the predicate, else [0, 1]
//   select               the arm's range when the condition's range is one
//                        value, else the union (integer results only)
//   const                the value
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/Interfaces/Utils/InferIntRangeCommon.h"

using namespace mlir;
using namespace mlir::intrange;

namespace sea {

namespace {

OverflowFlags flagsOf(bool nsw, bool nuw) {
  OverflowFlags f = OverflowFlags::None;
  if (nsw)
    f |= OverflowFlags::Nsw;
  if (nuw)
    f |= OverflowFlags::Nuw;
  return f;
}

bool mayBeZero(const ConstantIntRanges &r) { return r.umin().isZero(); }

CmpPredicate toIntRange(ICmpPredicate p) {
  switch (p) {
  case ICmpPredicate::eq:
    return CmpPredicate::eq;
  case ICmpPredicate::ne:
    return CmpPredicate::ne;
  case ICmpPredicate::slt:
    return CmpPredicate::slt;
  case ICmpPredicate::sle:
    return CmpPredicate::sle;
  case ICmpPredicate::sgt:
    return CmpPredicate::sgt;
  case ICmpPredicate::sge:
    return CmpPredicate::sge;
  case ICmpPredicate::ult:
    return CmpPredicate::ult;
  case ICmpPredicate::ule:
    return CmpPredicate::ule;
  case ICmpPredicate::ugt:
    return CmpPredicate::ugt;
  case ICmpPredicate::uge:
    return CmpPredicate::uge;
  }
  llvm_unreachable("unknown icmp predicate");
}

} // namespace

void ConstOp::inferResultRanges(ArrayRef<ConstantIntRanges>,
                                SetIntRangeFn setResultRange) {
  if (auto a = dyn_cast<IntegerAttr>(getValue()))
    setResultRange(getResult(), ConstantIntRanges::constant(a.getValue()));
}

#define SEA_WRAP_RANGE(Op, fn)                                                 \
  void Op::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,            \
                             SetIntRangeFn setResultRange) {                   \
    setResultRange(getResult(), fn(argRanges, flagsOf(getNsw(), getNuw())));   \
  }
SEA_WRAP_RANGE(AddOp, inferAdd)
SEA_WRAP_RANGE(SubOp, inferSub)
SEA_WRAP_RANGE(MulOp, inferMul)
#undef SEA_WRAP_RANGE

void ShlOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                              SetIntRangeFn setResultRange) {
  // a shift by >= width keeps a promise when x = 0 (sea: the value 0), while
  // upstream saturates it under a flag (ushl_sat / sshl_sat): the flags are
  // used only when every shift amount is below the width
  unsigned w = argRanges[1].umax().getBitWidth();
  bool inWidth = argRanges[1].umax().ult(w);
  setResultRange(getResult(),
                 inferShl(argRanges, inWidth ? flagsOf(getNsw(), getNuw())
                                             : OverflowFlags::None));
}

#define SEA_TOTAL_RANGE(Op, fn)                                                \
  void Op::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,            \
                             SetIntRangeFn setResultRange) {                   \
    setResultRange(getResult(), fn(argRanges));                                \
  }
SEA_TOTAL_RANGE(SDivOp, inferDivS)
SEA_TOTAL_RANGE(SRemOp, inferRemS)
SEA_TOTAL_RANGE(AndOp, inferAnd)
SEA_TOTAL_RANGE(OrOp, inferOr)
SEA_TOTAL_RANGE(XorOp, inferXor)
SEA_TOTAL_RANGE(LShrOp, inferShrU)
SEA_TOTAL_RANGE(AShrOp, inferShrS)
SEA_TOTAL_RANGE(UMinOp, inferMinU)
SEA_TOTAL_RANGE(UMaxOp, inferMaxU)
SEA_TOTAL_RANGE(SMinOp, inferMinS)
SEA_TOTAL_RANGE(SMaxOp, inferMaxS)
#undef SEA_TOTAL_RANGE

void UDivOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                               SetIntRangeFn setResultRange) {
  ConstantIntRanges r = inferDivU(argRanges);
  // bvudiv x 0 = ~0
  if (mayBeZero(argRanges[1]))
    r = r.rangeUnion(ConstantIntRanges::constant(
        APInt::getAllOnes(argRanges[1].umin().getBitWidth())));
  setResultRange(getResult(), r);
}

void URemOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                               SetIntRangeFn setResultRange) {
  ConstantIntRanges r = inferRemU(argRanges);
  // bvurem x 0 = x
  if (mayBeZero(argRanges[1]))
    r = r.rangeUnion(argRanges[0]);
  setResultRange(getResult(), r);
}

void ZExtOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                               SetIntRangeFn setResultRange) {
  setResultRange(getResult(),
                 extUIRange(argRanges[0], bitWidthOf(getResult().getType())));
}

void SExtOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                               SetIntRangeFn setResultRange) {
  setResultRange(getResult(),
                 extSIRange(argRanges[0], bitWidthOf(getResult().getType())));
}

void TruncOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                                SetIntRangeFn setResultRange) {
  setResultRange(getResult(),
                 truncRange(argRanges[0], bitWidthOf(getResult().getType())));
}

void ICmpOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                               SetIntRangeFn setResultRange) {
  std::optional<bool> v =
      evaluatePred(toIntRange(getPredicate()), argRanges[0], argRanges[1]);
  if (v)
    setResultRange(getResult(), ConstantIntRanges::constant(APInt(1, *v)));
  else
    setResultRange(getResult(), ConstantIntRanges::maxRange(1));
}

void SelectOp::inferResultRanges(ArrayRef<ConstantIntRanges> argRanges,
                                 SetIntRangeFn setResultRange) {
  // pointer / address / struct selects have no integer range
  if (!isa<IntegerType>(getResult().getType()))
    return;
  if (std::optional<APInt> c = argRanges[0].getConstantValue()) {
    setResultRange(getResult(), c->isOne() ? argRanges[1] : argRanges[2]);
    return;
  }
  setResultRange(getResult(), argRanges[1].rangeUnion(argRanges[2]));
}

} // namespace sea
