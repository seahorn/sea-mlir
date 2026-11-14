// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Semantics.cpp - concrete semantics of sea ops ------------*- C++ -*-===//

#include "sea/Dialect/Sea/Semantics.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/Sea/SeaTypes.h"

#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/MathExtras.h"

using namespace mlir;
using llvm::APInt;

namespace sea {

APInt bvUDiv(const APInt &x, const APInt &y) {
  if (y.isZero())
    return APInt::getAllOnes(x.getBitWidth());
  return x.udiv(y);
}

APInt bvURem(const APInt &x, const APInt &y) {
  if (y.isZero())
    return x;
  return x.urem(y);
}

// SMT-LIB defines bvsdiv and bvsrem by cases on the sign bits, in terms of
// bvudiv/bvurem and bvneg. Follow the definition literally.
APInt bvSDiv(const APInt &x, const APInt &y) {
  bool nx = x.isNegative(), ny = y.isNegative();
  if (!nx && !ny)
    return bvUDiv(x, y);
  if (nx && !ny)
    return -bvUDiv(-x, y);
  if (!nx && ny)
    return -bvUDiv(x, -y);
  return bvUDiv(-x, -y);
}

APInt bvSRem(const APInt &x, const APInt &y) {
  bool nx = x.isNegative(), ny = y.isNegative();
  if (!nx && !ny)
    return bvURem(x, y);
  if (nx && !ny)
    return -bvURem(-x, y);
  if (!nx && ny)
    return bvURem(x, -y);
  return -bvURem(-x, -y);
}

APInt bvShl(const APInt &x, const APInt &y) {
  if (y.uge(x.getBitWidth()))
    return APInt::getZero(x.getBitWidth());
  return x.shl(y.getZExtValue());
}

APInt bvLShr(const APInt &x, const APInt &y) {
  if (y.uge(x.getBitWidth()))
    return APInt::getZero(x.getBitWidth());
  return x.lshr(y.getZExtValue());
}

APInt bvAShr(const APInt &x, const APInt &y) {
  if (y.uge(x.getBitWidth()))
    return x.isNegative() ? APInt::getAllOnes(x.getBitWidth())
                          : APInt::getZero(x.getBitWidth());
  return x.ashr(y.getZExtValue());
}

static bool evalICmp(ICmpPredicate p, const APInt &x, const APInt &y) {
  switch (p) {
  case ICmpPredicate::eq:
    return x == y;
  case ICmpPredicate::ne:
    return x != y;
  case ICmpPredicate::slt:
    return x.slt(y);
  case ICmpPredicate::sle:
    return x.sle(y);
  case ICmpPredicate::sgt:
    return x.sgt(y);
  case ICmpPredicate::sge:
    return x.sge(y);
  case ICmpPredicate::ult:
    return x.ult(y);
  case ICmpPredicate::ule:
    return x.ule(y);
  case ICmpPredicate::ugt:
    return x.ugt(y);
  case ICmpPredicate::uge:
    return x.uge(y);
  }
  llvm_unreachable("unknown icmp predicate");
}

/// The overflow bit of LLVM's *.with.overflow intrinsics (APInt's *_ov).
template <APInt (APInt::*Op)(const APInt &, bool &) const>
static APInt overflows(const APInt &x, const APInt &y) {
  bool ov = false;
  (void)(x.*Op)(y, ov);
  return APInt(1, ov);
}

static unsigned resultWidth(Operation *op) {
  return op->getResult(0).getType().getIntOrFloatBitWidth();
}

std::optional<APInt> evalPureOp(Operation *op, ArrayRef<APInt> v) {
  return llvm::TypeSwitch<Operation *, std::optional<APInt>>(op)
      .Case([](ConstOp c) {
        return llvm::cast<IntegerAttr>(c.getValue()).getValue();
      })
      .Case([&](AddOp) { return v[0] + v[1]; })
      .Case([&](SubOp) { return v[0] - v[1]; })
      .Case([&](MulOp) { return v[0] * v[1]; })
      .Case([&](UDivOp) { return bvUDiv(v[0], v[1]); })
      .Case([&](SDivOp) { return bvSDiv(v[0], v[1]); })
      .Case([&](URemOp) { return bvURem(v[0], v[1]); })
      .Case([&](SRemOp) { return bvSRem(v[0], v[1]); })
      .Case([&](AndOp) { return v[0] & v[1]; })
      .Case([&](OrOp) { return v[0] | v[1]; })
      .Case([&](XorOp) { return v[0] ^ v[1]; })
      .Case([&](ShlOp) { return bvShl(v[0], v[1]); })
      .Case([&](LShrOp) { return bvLShr(v[0], v[1]); })
      .Case([&](AShrOp) { return bvAShr(v[0], v[1]); })
      .Case([&](ICmpOp c) {
        return APInt(1, evalICmp(c.getPredicate(), v[0], v[1]));
      })
      .Case([&](SelectOp) { return v[0].isOne() ? v[1] : v[2]; })
      // Addresses are bit-vectors; only these ops accept them.
      .Case([&](AddrAddOp) { return v[0] + v[1]; })
      .Case([&](AddrDiffOp) { return v[0] - v[1]; })
      .Case([&](AddrCmpOp c) {
        return APInt(1, evalICmp(c.getPredicate(), v[0], v[1]));
      })
      // A pointer compare observes the addresses (ADDR, PtrSort.md): the
      // operands are the two addresses (the interpreter supplies them for
      // abstract pointers).
      .Case([&](PtrCmpOp c) {
        return APInt(1, evalICmp(c.getPredicate(), v[0], v[1]));
      })
      .Case([&](PtrIndexOp p) -> std::optional<APInt> {
        // an address base; an abstract pointer is the interpreter's
        if (!isa<AddrType>(p.getBase().getType()))
          return std::nullopt;
        return v[0] + ptrIndexOffset(p, v.drop_front());
      })
      .Case([&](AddrToIntOp) { return v[0]; })
      .Case([&](AddrFromIntOp) { return v[0]; })
      .Case([&](AddrToBitsOp) { return v[0]; })
      .Case([&](AddrFromBitsOp) { return v[0]; })
      // A placed object's base address. The op has no folder on purpose
      // (sccp would drop the provenance, SeaM); the interpreter gets its
      // value here.
      .Case([](SegmentOp s) { return s.getBase(); })
      .Case([&](ZExtOp z) { return v[0].zext(resultWidth(z)); })
      .Case([&](SExtOp s) { return v[0].sext(resultWidth(s)); })
      .Case([&](TruncOp t) { return v[0].trunc(resultWidth(t)); })
      .Case([&](ConcatOp) { return v[0].concat(v[1]); })
      .Case([&](ExtractOp e) {
        return v[0].extractBits(resultWidth(e), e.getLow());
      })
      // Intrinsics (LLVM's own APInt implementations of them).
      .Case([&](UMinOp) { return llvm::APIntOps::umin(v[0], v[1]); })
      .Case([&](UMaxOp) { return llvm::APIntOps::umax(v[0], v[1]); })
      .Case([&](SMinOp) { return llvm::APIntOps::smin(v[0], v[1]); })
      .Case([&](SMaxOp) { return llvm::APIntOps::smax(v[0], v[1]); })
      .Case([&](BSwapOp) { return v[0].byteSwap(); })
      .Case([&](CtPopOp) { return APInt(v[0].getBitWidth(), v[0].popcount()); })
      // INT_MIN is its own absolute value (the negation wraps)
      .Case([&](AbsOp) { return v[0].abs(); })
      .Case([&](FShlOp) { return llvm::APIntOps::fshl(v[0], v[1], v[2]); })
      .Case([&](FShrOp) { return llvm::APIntOps::fshr(v[0], v[1], v[2]); })
      .Case([&](UAddOOp) { return overflows<&APInt::uadd_ov>(v[0], v[1]); })
      .Case([&](SAddOOp) { return overflows<&APInt::sadd_ov>(v[0], v[1]); })
      .Case([&](USubOOp) { return overflows<&APInt::usub_ov>(v[0], v[1]); })
      .Case([&](SSubOOp) { return overflows<&APInt::ssub_ov>(v[0], v[1]); })
      .Case([&](UMulOOp) { return overflows<&APInt::umul_ov>(v[0], v[1]); })
      .Case([&](SMulOOp) { return overflows<&APInt::smul_ov>(v[0], v[1]); })
      // Not APInt::sdiv_ov: it divides, and x / 0 must be total here.
      .Case([&](SDivOOp) {
        return APInt(1, v[0].isMinSignedValue() && v[1].isAllOnes());
      })
      .Case([&](NegOOp) { return APInt(1, v[0].isMinSignedValue()); })
      .Default([](Operation *) { return std::nullopt; });
}

std::optional<StringRef> brokenWrapPromise(Operation *op, ArrayRef<APInt> v) {
  bool nsw = op->hasAttr("nsw"), nuw = op->hasAttr("nuw");
  if (!nsw && !nuw)
    return std::nullopt;
  bool sov = false, uov = false;
  if (isa<AddOp>(op)) {
    (void)v[0].sadd_ov(v[1], sov);
    (void)v[0].uadd_ov(v[1], uov);
  } else if (isa<SubOp>(op)) {
    (void)v[0].ssub_ov(v[1], sov);
    (void)v[0].usub_ov(v[1], uov);
  } else if (isa<MulOp>(op)) {
    (void)v[0].smul_ov(v[1], sov);
    (void)v[0].umul_ov(v[1], uov);
  } else if (isa<AbsOp>(op)) {
    // nsw = LLVM's is_int_min_poison: 0 - x does not overflow
    sov = v[0].isMinSignedValue();
  } else if (isa<ShlOp>(op)) {
    // LangRef: nuw (nsw) promises that no non-zero bits (no bits that
    // disagree with the sign) are shifted out; the shift by >= width is
    // poison anyway and counts as broken.
    if (v[1].uge(v[0].getBitWidth())) {
      sov = uov = !v[0].isZero();
    } else {
      unsigned s = v[1].getZExtValue();
      APInt r = v[0].shl(s);
      sov = r.ashr(s) != v[0];
      uov = r.lshr(s) != v[0];
    }
  } else {
    return std::nullopt;
  }
  if (nsw && sov)
    return StringRef("nsw");
  if (nuw && uov)
    return StringRef("nuw");
  return std::nullopt;
}

/// A stride or the offset of a ptr.index: an n-bit two's complement value
/// written as its 64-bit sign extension.
static APInt ptrIndexConst(int64_t v, unsigned n) {
  APInt a(64, uint64_t(v), /*isSigned=*/true);
  return n < 64 ? a.trunc(n) : a;
}

APInt ptrIndexOffset(Operation *op, ArrayRef<APInt> idx) {
  auto p = cast<PtrIndexOp>(op);
  unsigned n = bitWidthOf(p.getBase().getType());
  APInt off = ptrIndexConst(p.getOffset(), n);
  for (auto [k, i] : llvm::enumerate(idx)) {
    APInt x = i.getBitWidth() < n ? i.sext(n) : i;
    off += x * ptrIndexConst(p.getStrides()[k], n);
  }
  return off;
}

std::optional<StringRef> brokenPtrIndexPromise(Operation *op,
                                               std::optional<APInt> base,
                                               ArrayRef<APInt> idx) {
  auto p = cast<PtrIndexOp>(op);
  bool nusw = p.getInbounds() || p.getNusw(), nuw = p.getNuw();
  unsigned n = bitWidthOf(p.getBase().getType());
  // exact integers: products of two n-bit values, a sum of m of them and c,
  // and a base: 2n + 8 bits are plenty for m < 2^7 (more indices: wider)
  unsigned w = 2 * n + 8 + unsigned(llvm::Log2_64_Ceil(idx.size() + 2));
  auto check = [&](bool isSigned) -> std::optional<StringRef> {
    auto ext = [&](const APInt &x) { return isSigned ? x.sext(w) : x.zext(w); };
    auto fits = [&](const APInt &x) {
      return isSigned ? x.isSignedIntN(n) : (x.isNonNegative() && x.isIntN(n));
    };
    APInt total = ext(ptrIndexConst(p.getOffset(), n));
    for (auto [k, i] : llvm::enumerate(idx)) {
      // the index is sign-extended to n bits in both cases (LangRef: nuw
      // reads the extended index as unsigned)
      APInt x = i.getBitWidth() < n ? i.sext(n) : i;
      APInt prod = ext(x) * ext(ptrIndexConst(p.getStrides()[k], n));
      if (!fits(prod))
        return StringRef(isSigned ? "nusw: a product wraps (P1)"
                                  : "nuw: a product wraps (P1)");
      total += prod;
    }
    if (!fits(total))
      return StringRef(isSigned ? "nusw: the offset wraps (P2)"
                                : "nuw: the offset wraps (P2)");
    if (base) {
      APInt a = base->zext(w) + total;
      if (!(a.isNonNegative() && a.isIntN(n)))
        return StringRef(isSigned ? "nusw: base + offset leaves the space (P3)"
                                  : "nuw: base + offset leaves the space (P3)");
    }
    return std::nullopt;
  };
  if (nusw)
    if (auto b = check(true))
      return b;
  if (nuw)
    if (auto b = check(false))
      return b;
  return std::nullopt;
}

APInt boxTripCount(const APInt &lo, const APInt &step, const APInt &ub) {
  if (ub.ule(lo))
    return APInt::getZero(ub.getBitWidth());
  return (ub - lo - 1).udiv(step) + 1;
}

} // namespace sea
