// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The low k bits of a bit-vector term, computed at k bits
// (docs/design/Ackermann.md, "Narrowed offsets").
//
// LowBits::of(e, k) is a k-bit term equal to extract(k-1, 0, e) for every
// assignment. Truncation to the low k bits is a ring homomorphism from
// bv(w) to bv(k) for +, -, *, negation, and commutes with the bitwise
// operators, ite (on its branches), and shifts left by a numeral; the low
// k bits of zext / sext / concat / extract are read off directly. So the
// arithmetic is rebuilt at k bits from truncated operands instead of being
// extracted from a w-bit result: a w-bit adder becomes a k-bit one in the
// bit-blast.
//
// Each step is an identity of bit-vector arithmetic (no assumption):
//   low_k(a + b) = low_k(a) + low_k(b), same for -, * and bvneg;
//   low_k(a op b) = low_k(a) op low_k(b) for bvand, bvor, bvxor; bvnot;
//   low_k(ite(c, a, b)) = ite(c, low_k(a), low_k(b));
//   low_k(a << s) = low_k(a) << s for a numeral s < k, 0 for s >= k;
//   low_k(zext(x)) = low_k(x) if width(x) >= k, else zext(x) to k bits;
//   low_k(sext(x)) the same with sext;
//   low_k(concat(h, l)) = low_k(l) if width(l) >= k, else
//     concat(low_{k - width(l)}(h), l);
//   low_k(extract(h, l, x)) = low_k(x) for l = 0, else extract(l+k-1, l, x);
//   a numeral: its value modulo 2^k; a term of width k: itself.
// Other terms are extract(k-1, 0, e). The bitwise operators and ite are
// pushed through only when an operand holds arithmetic (+, -, *, bvneg):
// elsewhere the extract is free in the bit-blast and pushing would only
// duplicate structure. Memoized per (node, k); the memo keeps its keys alive.
#include "sea/Expr/Expr.hh"
#include "sea/Expr/TypeChecker.hh"

#include "llvm/ADT/SmallVector.h"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {

class LowBits {
public:
  /// The low \p k bits of the bit-vector term \p e, as a k-bit term
  /// (0 < k <= width(e)).
  Expr of(const Expr &e, unsigned k) {
    unsigned w = width(e);
    assert(k > 0 && (w == 0 || k <= w) && "LowBits: k out of range");
    if (w == k)
      return e;
    auto key = std::make_pair(e.get(), k);
    auto it = m_memo.find(key);
    if (it != m_memo.end())
      return it->second;
    Expr r = build(e, k);
    m_keep.push_back(e);
    m_memo.emplace(key, r);
    return r;
  }

  /// The width of a bit-vector term (0: not a bit-vector or unknown).
  unsigned width(const Expr &e) {
    unsigned w;
    if (op::bv::isBvNum(e, w))
      return w;
    if (isOpX<FAPP>(e) && e->arity() == 1 && isOpX<FDECL>(e->first())) {
      Expr s = op::bind::rangeTy(e->first());
      return isOpX<op::BVSORT>(s) ? op::bv::width(s) : 0;
    }
    auto it = m_widths.find(e.get());
    if (it != m_widths.end())
      return it->second;
    w = computeWidth(e);
    m_widths.emplace(e.get(), w);
    m_keep.push_back(e);
    return w;
  }

  /// Whether \p e holds arithmetic (+, -, *, bvneg) below bitwise operators,
  /// ite, extensions, concat or extract (the operators of() pushes through).
  bool hasArith(const Expr &e) {
    auto it = m_arith.find(e.get());
    if (it != m_arith.end())
      return it->second;
    bool r = false;
    if (isOpX<BADD>(e) || isOpX<BSUB>(e) || isOpX<BMUL>(e) || isOpX<BNEG>(e))
      r = true;
    else if (isOpX<BAND>(e) || isOpX<BOR>(e) || isOpX<BXOR>(e) ||
             isOpX<BNOT>(e) || isOpX<BCONCAT>(e) || isOpX<BSHL>(e)) {
      for (size_t i = 0; i < e->arity() && !r; ++i)
        r = hasArith(Expr(e->arg(i)));
    } else if (isOpX<ITE>(e)) {
      r = hasArith(Expr(e->arg(1))) || hasArith(Expr(e->arg(2)));
    } else if (isOpX<BZEXT>(e) || isOpX<BSEXT>(e)) {
      r = hasArith(Expr(e->arg(0)));
    } else if (isOpX<BEXTRACT>(e)) {
      r = hasArith(op::bv::earg(e));
    }
    m_arith.emplace(e.get(), r);
    m_keep.push_back(e);
    return r;
  }

private:
  Expr num(const mpz_class &v, unsigned k, ExprFactory &ef) {
    mpz_class m = 1L;
    mpz_mul_2exp(m.get_mpz_t(), m.get_mpz_t(), k);
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), m.get_mpz_t());
    return op::bv::bvnum(r, k, ef);
  }

  Expr cut(const Expr &e, unsigned k) { return op::bv::extract(k - 1, 0, e); }

  Expr build(const Expr &e, unsigned k) {
    ExprFactory &ef = e->efac();
    if (op::bv::isBvNum(e))
      return num(op::bv::toMpz(e), k, ef);
    if (isOpX<BADD>(e) || isOpX<BSUB>(e) || isOpX<BMUL>(e)) {
      llvm::SmallVector<Expr, 8> kids;
      for (size_t i = 0; i < e->arity(); ++i)
        kids.push_back(of(Expr(e->arg(i)), k));
      if (kids.size() == 1)
        return kids[0];
      return ef.mkNary(e->op(), kids.begin(), kids.end());
    }
    if (isOpX<BNEG>(e))
      return mk<BNEG>(of(Expr(e->arg(0)), k));
    if ((isOpX<BAND>(e) || isOpX<BOR>(e) || isOpX<BXOR>(e)) && hasArith(e)) {
      llvm::SmallVector<Expr, 8> kids;
      for (size_t i = 0; i < e->arity(); ++i)
        kids.push_back(of(Expr(e->arg(i)), k));
      if (kids.size() == 1)
        return kids[0];
      return ef.mkNary(e->op(), kids.begin(), kids.end());
    }
    if (isOpX<BNOT>(e) && hasArith(e))
      return mk<BNOT>(of(Expr(e->arg(0)), k));
    if (isOpX<ITE>(e) && hasArith(e))
      return mk<ITE>(Expr(e->arg(0)), of(Expr(e->arg(1)), k),
                     of(Expr(e->arg(2)), k));
    if (isOpX<BSHL>(e) && e->arity() == 2 && op::bv::isBvNum(Expr(e->arg(1)))) {
      mpz_class s = op::bv::toMpz(Expr(e->arg(1)));
      if (s < 0L || s >= mpz_class((unsigned long)k))
        return num(mpz_class(0L), k, ef);
      if (s == 0L)
        return of(Expr(e->arg(0)), k);
      return mk<BSHL>(of(Expr(e->arg(0)), k), num(s, k, ef));
    }
    if ((isOpX<BZEXT>(e) || isOpX<BSEXT>(e)) && e->arity() == 2) {
      Expr x(e->arg(0));
      unsigned wx = width(x);
      if (!wx)
        return cut(e, k);
      if (wx >= k)
        return of(x, k);
      return isOpX<BZEXT>(e) ? op::bv::zext(x, k) : op::bv::sext(x, k);
    }
    if (isOpX<BCONCAT>(e) && e->arity() == 2) {
      Expr hi(e->arg(0)), lo(e->arg(1));
      unsigned wl = width(lo);
      if (!wl || !width(hi))
        return cut(e, k);
      if (wl >= k)
        return of(lo, k);
      return op::bv::concat(of(hi, k - wl), lo);
    }
    if (isOpX<BEXTRACT>(e)) {
      unsigned l = op::bv::low(e);
      if (l == 0)
        return of(op::bv::earg(e), k);
      return op::bv::extract(l + k - 1, l, op::bv::earg(e));
    }
    return cut(e, k);
  }

  unsigned computeWidth(const Expr &e) {
    if (isOpX<BEXTRACT>(e))
      return op::bv::high(e) - op::bv::low(e) + 1;
    if (isOpX<BCONCAT>(e)) {
      unsigned s = 0;
      for (size_t i = 0; i < e->arity(); ++i) {
        unsigned w = width(Expr(e->arg(i)));
        if (!w)
          return 0;
        s += w;
      }
      return s;
    }
    if (isOpX<BADD>(e) || isOpX<BSUB>(e) || isOpX<BMUL>(e) || isOpX<BAND>(e) ||
        isOpX<BOR>(e) || isOpX<BXOR>(e) || isOpX<BNOT>(e) || isOpX<BNEG>(e) ||
        isOpX<BUDIV>(e) || isOpX<BSDIV>(e) || isOpX<BUREM>(e) ||
        isOpX<BSREM>(e) || isOpX<BSMOD>(e) || isOpX<BSHL>(e) ||
        isOpX<BLSHR>(e) || isOpX<BASHR>(e))
      return width(Expr(e->arg(0)));
    if (isOpX<ITE>(e))
      return width(Expr(e->arg(1)));
    if (isOpX<BZEXT>(e) || isOpX<BSEXT>(e))
      return isOpX<op::BVSORT>(e->arg(1)) ? op::bv::width(Expr(e->arg(1))) : 0;
    Expr t = m_tc.typeOf(e);
    return isOpX<op::BVSORT>(t) ? op::bv::width(t) : 0;
  }

  std::map<std::pair<ENode *, unsigned>, Expr> m_memo;
  std::unordered_map<ENode *, unsigned> m_widths;
  std::unordered_map<ENode *, bool> m_arith;
  std::vector<Expr> m_keep;
  TypeChecker m_tc;
};

} // namespace expr
