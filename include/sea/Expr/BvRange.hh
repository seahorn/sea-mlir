// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Structural ranges of bit-vector terms (docs/design/Ackermann.md, "The
// difference decider").
//
// BvRanges::repr(e) is an integer interval [lo, hi] that contains, for every
// assignment, the integer representative rep(e) of the w-bit value of e
// (w = width(e)): rep(e) = uval(e) (mod 2^w). The representative is the
// unsigned value except where a signed reading is free:
//   - a numeral of value v >= 2^(w-1): v - 2^w (its signed value);
//   - sext(x): the signed value of x;
//   - bvadd / bvsub / bvneg / bvmul by a numeral: the integer sum / product
//     of the operands' representatives (no reduction modulo 2^w);
//   - ite: the representative of the chosen branch (the hull of both).
// Everything else is read as its unsigned value. unsignedRange(e) is the
// interval of uval(e) itself: repr(e) shifted into [0, 2^w) when it lies in
// one window [k 2^w, (k + 1) 2^w), else [0, 2^w - 1].
//
// Each rule is an identity or a bound of SMT-LIB bit-vector arithmetic,
// total semantics included (bvurem x 0 = x, bvudiv x 0 = ~0, shifts by
// >= w give 0); no assumption. Constants with a known value (the layout,
// obj.k = <numeral>: a conjunct of the query) are points when \p values is
// given; the caller states that premise. A term whose width is unknown has no
// range (known = false). Memoized per node; the memo keeps its keys alive.
//
// Operators: numerals, constants (full range at their width), zext, sext,
// concat, extract, bvand, bvor, bvurem, bvudiv, bvlshr / bvshl by a numeral,
// bvadd, bvsub, bvneg, bvmul, ite. Other terms: the full range.
#include "sea/Expr/Expr.hh"
#include "sea/Expr/LowBits.hh"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace expr {

namespace bvz {
// The arithmetic of mpz_class (ExprGmp.hh has few operators).
inline mpz_class add(const mpz_class &a, const mpz_class &b) {
  mpz_class r;
  mpz_add(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return r;
}
inline mpz_class sub(const mpz_class &a, const mpz_class &b) {
  mpz_class r;
  mpz_sub(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return r;
}
inline mpz_class mul(const mpz_class &a, const mpz_class &b) {
  mpz_class r;
  mpz_mul(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return r;
}
inline mpz_class neg(const mpz_class &a) {
  mpz_class r;
  mpz_neg(r.get_mpz_t(), a.get_mpz_t());
  return r;
}
/// floor(a / b), b > 0
inline mpz_class fdiv(const mpz_class &a, const mpz_class &b) {
  mpz_class r;
  mpz_fdiv_q(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return r;
}
/// a mod b in [0, b), b > 0
inline mpz_class fmod(const mpz_class &a, const mpz_class &b) {
  mpz_class r;
  mpz_fdiv_r(r.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return r;
}
inline mpz_class pow2(unsigned w) {
  mpz_class m;
  mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
  return m;
}
inline const mpz_class &min(const mpz_class &a, const mpz_class &b) {
  return b < a ? b : a;
}
inline const mpz_class &max(const mpz_class &a, const mpz_class &b) {
  return a < b ? b : a;
}
/// The symmetric representative of v modulo 2^w, in [-2^(w-1), 2^(w-1)).
inline mpz_class symmetric(const mpz_class &v, unsigned w) {
  mpz_class m = pow2(w);
  mpz_class r = fmod(v, m);
  mpz_class r2;
  mpz_mul_2exp(r2.get_mpz_t(), r.get_mpz_t(), 1);
  if (r2 >= m)
    r = sub(r, m);
  return r;
}
} // namespace bvz

/// An integer interval [lo, hi], lo <= hi; known = false: no bound.
struct BvInterval {
  mpz_class lo, hi;
  bool known = false;
};

class BvRanges {
public:
  explicit BvRanges(
      const std::unordered_map<ENode *, mpz_class> *values = nullptr)
      : m_values(values) {}

  /// The interval of rep(e) (see the header).
  const BvInterval &repr(const Expr &e) {
    auto it = m_memo.find(e.get());
    if (it != m_memo.end())
      return it->second;
    BvInterval r = compute(e);
    m_keep.push_back(e);
    return m_memo.emplace(e.get(), std::move(r)).first->second;
  }

  /// The interval of the unsigned value of \p e, within [0, 2^w).
  BvInterval unsignedRange(const Expr &e) {
    unsigned w = width(e);
    const BvInterval &r = repr(e);
    if (!r.known || !w)
      return BvInterval();
    return reduce(r, w);
  }

  /// The width of a bit-vector term (0: unknown).
  unsigned width(const Expr &e) { return m_low.width(e); }

  /// [lo - k 2^w, hi - k 2^w] if [lo, hi] lies in [k 2^w, (k + 1) 2^w), else
  /// [0, 2^w - 1]: the unsigned values of the w-bit residues of [lo, hi].
  static BvInterval reduce(const BvInterval &r, unsigned w) {
    mpz_class m = bvz::pow2(w);
    mpz_class base = bvz::mul(bvz::fdiv(r.lo, m), m);
    if (bvz::sub(r.hi, base) < m)
      return {bvz::sub(r.lo, base), bvz::sub(r.hi, base), true};
    return full(w);
  }

  static BvInterval full(unsigned w) {
    return {mpz_class(0L), bvz::sub(bvz::pow2(w), mpz_class(1L)), true};
  }

private:
  static BvInterval point(const mpz_class &v) { return {v, v, true}; }

  /// [lo, hi] of width at least 2^w says nothing beyond the residues: the
  /// full unsigned range instead (narrower, same residues).
  static BvInterval clamp(const BvInterval &r, unsigned w) {
    if (!w)
      return BvInterval();
    if (!r.known || bvz::sub(r.hi, r.lo) >= bvz::pow2(w))
      return full(w);
    return r;
  }

  static BvInterval add(const BvInterval &a, const BvInterval &b) {
    if (!a.known || !b.known)
      return BvInterval();
    return {bvz::add(a.lo, b.lo), bvz::add(a.hi, b.hi), true};
  }
  static BvInterval neg(const BvInterval &a) {
    if (!a.known)
      return a;
    return {bvz::neg(a.hi), bvz::neg(a.lo), true};
  }
  static BvInterval scale(const BvInterval &a, const mpz_class &k) {
    if (!a.known)
      return a;
    if (k >= 0L)
      return {bvz::mul(a.lo, k), bvz::mul(a.hi, k), true};
    return {bvz::mul(a.hi, k), bvz::mul(a.lo, k), true};
  }
  static mpz_class reducedNum(const Expr &num) {
    return bvz::fmod(op::bv::toMpz(num), bvz::pow2(op::bv::widthBvNum(num)));
  }

  BvInterval compute(const Expr &e) {
    unsigned w = width(e);
    if (!w)
      return BvInterval();
    unsigned nw;
    if (op::bv::isBvNum(e, nw))
      return point(bvz::symmetric(op::bv::toMpz(e), w));
    if (m_values) {
      auto it = m_values->find(e.get());
      if (it != m_values->end())
        return point(bvz::fmod(it->second, bvz::pow2(w)));
    }
    ENode *n = e.get();
    auto kid = [&](size_t i) { return Expr(n->arg(i)); };
    if (isOpX<BZEXT>(n)) {
      BvInterval x = unsignedRange(kid(0));
      return x.known ? x : full(w);
    }
    if (isOpX<BSEXT>(n)) {
      // the signed value of x
      unsigned xw = width(kid(0));
      BvInterval x = unsignedRange(kid(0));
      if (!x.known || !xw)
        return full(w);
      mpz_class half = bvz::pow2(xw - 1), m = bvz::pow2(xw);
      if (x.hi < half)
        return x;
      if (x.lo >= half)
        return {bvz::sub(x.lo, m), bvz::sub(x.hi, m), true};
      return {bvz::neg(half), bvz::sub(half, mpz_class(1L)), true};
    }
    if (isOpX<BCONCAT>(n)) {
      // the first operand is the high part; each bound is monotone in every
      // part
      BvInterval r = point(mpz_class(0L));
      for (size_t i = 0; i < n->arity(); ++i) {
        unsigned kw = width(kid(i));
        BvInterval x = unsignedRange(kid(i));
        if (!kw || !x.known)
          return full(w);
        mpz_class m = bvz::pow2(kw);
        r = {bvz::add(bvz::mul(r.lo, m), x.lo),
             bvz::add(bvz::mul(r.hi, m), x.hi), true};
      }
      return r;
    }
    if (isOpX<BEXTRACT>(n)) {
      // bits [l, h] of x < 2^(h+1): floor(x / 2^l)
      unsigned h = op::bv::high(e), l = op::bv::low(e);
      BvInterval x = unsignedRange(op::bv::earg(e));
      if (!x.known || x.hi >= bvz::pow2(h + 1))
        return full(w);
      mpz_class lo, hi;
      mpz_fdiv_q_2exp(lo.get_mpz_t(), x.lo.get_mpz_t(), l);
      mpz_fdiv_q_2exp(hi.get_mpz_t(), x.hi.get_mpz_t(), l);
      return {lo, hi, true};
    }
    if (isOpX<BAND>(n)) {
      // x & y <= min(x, y)
      mpz_class hi = bvz::sub(bvz::pow2(w), mpz_class(1L));
      for (size_t i = 0; i < n->arity(); ++i) {
        BvInterval x = unsignedRange(kid(i));
        if (x.known && x.hi < hi)
          hi = x.hi;
      }
      return {mpz_class(0L), hi, true};
    }
    if (isOpX<BOR>(n)) {
      // max(x, y) <= x | y < 2^bits(max(hi))
      mpz_class lo(0L), hi(0L);
      for (size_t i = 0; i < n->arity(); ++i) {
        BvInterval x = unsignedRange(kid(i));
        if (!x.known)
          return full(w);
        lo = bvz::max(lo, x.lo);
        hi = bvz::max(hi, x.hi);
      }
      unsigned bits = hi == mpz_class(0L) ? 0 : unsigned(hi.sizeInBase(2));
      return {lo, bvz::sub(bvz::pow2(bits), mpz_class(1L)), true};
    }
    if (isOpX<BUREM>(n) && n->arity() == 2) {
      // x urem d <= x (also for d = 0: x), < d for d > 0
      BvInterval x = unsignedRange(kid(0)), d = unsignedRange(kid(1));
      if (!x.known)
        return full(w);
      mpz_class hi = x.hi;
      if (d.known && d.lo > 0L)
        hi = bvz::min(hi, bvz::sub(d.hi, mpz_class(1L)));
      return {mpz_class(0L), hi, true};
    }
    if (isOpX<BUDIV>(n) && n->arity() == 2) {
      // d may be 0 (x udiv 0 = ~0): no bound then
      BvInterval x = unsignedRange(kid(0)), d = unsignedRange(kid(1));
      if (!x.known || !d.known || d.lo == mpz_class(0L))
        return full(w);
      return {bvz::fdiv(x.lo, d.hi), bvz::fdiv(x.hi, d.lo), true};
    }
    if ((isOpX<BLSHR>(n) || isOpX<BSHL>(n)) && n->arity() == 2 &&
        op::bv::isBvNum(kid(1), nw)) {
      mpz_class s = reducedNum(kid(1));
      if (s >= (unsigned long)w)
        return point(mpz_class(0L));
      unsigned k = unsigned(s.get_ui());
      BvInterval x = unsignedRange(kid(0));
      if (!x.known)
        return full(w);
      mpz_class lo, hi;
      if (isOpX<BLSHR>(n)) {
        mpz_fdiv_q_2exp(lo.get_mpz_t(), x.lo.get_mpz_t(), k);
        mpz_fdiv_q_2exp(hi.get_mpz_t(), x.hi.get_mpz_t(), k);
        return {lo, hi, true};
      }
      mpz_mul_2exp(lo.get_mpz_t(), x.lo.get_mpz_t(), k);
      mpz_mul_2exp(hi.get_mpz_t(), x.hi.get_mpz_t(), k);
      if (hi >= bvz::pow2(w))
        return full(w);
      return {lo, hi, true};
    }
    if (isOpX<BADD>(n) || isOpX<BSUB>(n)) {
      BvInterval r = repr(kid(0));
      for (size_t i = 1; i < n->arity() && r.known; ++i)
        r = add(r, isOpX<BSUB>(n) ? neg(repr(kid(i))) : repr(kid(i)));
      return clamp(r, w);
    }
    if (isOpX<BNEG>(n) && n->arity() == 1)
      return clamp(neg(repr(kid(0))), w);
    if (isOpX<BMUL>(n)) {
      // numerals times one other factor: the integer product; else the
      // product of the unsigned ranges (a nonnegative numeral factor)
      mpz_class k(1L);
      llvm::SmallVector<Expr, 2> others;
      for (size_t i = 0; i < n->arity(); ++i) {
        if (op::bv::isBvNum(kid(i), nw))
          k = bvz::symmetric(bvz::mul(k, op::bv::toMpz(kid(i))), w);
        else
          others.push_back(kid(i));
      }
      if (others.empty())
        return point(k);
      if (others.size() == 1)
        return clamp(scale(repr(others[0]), k), w);
      if (k < 0L)
        return full(w);
      BvInterval r = point(k);
      for (const Expr &o : others) {
        BvInterval x = unsignedRange(o);
        if (!x.known)
          return full(w);
        r = {bvz::mul(r.lo, x.lo), bvz::mul(r.hi, x.hi), true};
      }
      return clamp(r, w);
    }
    if (isOpX<ITE>(n) && n->arity() == 3) {
      BvInterval t = repr(kid(1)), f = repr(kid(2));
      if (!t.known || !f.known)
        return full(w);
      return clamp({bvz::min(t.lo, f.lo), bvz::max(t.hi, f.hi), true}, w);
    }
    return full(w);
  }

  const std::unordered_map<ENode *, mpz_class> *m_values;
  LowBits m_low;
  std::unordered_map<ENode *, BvInterval> m_memo;
  std::vector<Expr> m_keep;
};

} // namespace expr
