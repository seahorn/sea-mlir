// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Linear forms of w-bit index terms (docs/design/Ackermann.md, "Pair
// reductions"): the Ackermann lowering's read sharing and pair tests,
// DiffDecider, SeaM's offset compares and InitialWords.
#include "sea/Expr/Expr.hh"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {
namespace ackermann_detail {
/// A bit-vector numeral's value reduced modulo 2^w (bvnum does not reduce;
/// a backend may return a signed numeral).
inline mpz_class reducedValue(const Expr &num) {
  unsigned w = op::bv::widthBvNum(num);
  mpz_class mod;
  mpz_ui_pow_ui(mod.get_mpz_t(), 2, w);
  mpz_class r;
  mpz_fdiv_r(r.get_mpz_t(), op::bv::toMpz(num).get_mpz_t(), mod.get_mpz_t());
  return r;
}

/// A linear form of a w-bit term: sum of coef(a) * a over atoms a, plus c,
/// modulo 2^w (coefficients nonzero, reduced).
struct Linear {
  std::map<ENode *, mpz_class> coef;
  mpz_class c;
};

/// The linear forms of index terms of one width w: bvadd, bvsub, bvneg, a
/// bvmul with one non-numeral factor, numerals; an ite whose two branches
/// have the same constant c is ite(cond, S1, S2) + c (an atom built from the
/// two symbolic parts, hash-consed, so equal structures meet); every other
/// term is an atom. Exact identities of w-bit arithmetic: the form is equal
/// to the term.
class LinearForms {
public:
  using Key = std::vector<std::pair<ENode *, mpz_class>>;

  explicit LinearForms(unsigned w) : m_w(w) {
    mpz_ui_pow_ui(m_mod.get_mpz_t(), 2, w);
  }
  /// Constants with a known value (the layout: obj.k = <numeral>, a fact of
  /// every model of the query): their form is that numeral. Set before the
  /// first of(); null: none (the default, used for read sharing).
  void setValues(const std::unordered_map<ENode *, mpz_class> *v) {
    m_values = v;
  }
  /// acc += sign * l (sign 1 or -1)
  void add(Linear &acc, const Linear &l, bool negate) const {
    mpz_class k(1UL);
    if (negate)
      mpz_sub_ui(k.get_mpz_t(), m_mod.get_mpz_t(), 1);
    addScaled(acc, l, k);
  }
  unsigned width() const { return m_w; }

  const Linear &of(ENode *n) {
    auto it = m_memo.find(n);
    if (it != m_memo.end())
      return it->second;
    Linear l = compute(n);
    m_keep.push_back(Expr(n));
    return m_memo.emplace(n, std::move(l)).first->second;
  }

  /// The symbolic part as a key (atoms ordered, with coefficients).
  Key key(const Linear &l) const { return Key(l.coef.begin(), l.coef.end()); }

private:
  mpz_class norm(const mpz_class &x) const {
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), x.get_mpz_t(), m_mod.get_mpz_t());
    return r;
  }
  mpz_class mul(const mpz_class &x, const mpz_class &y) const {
    mpz_class r;
    mpz_mul(r.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
    return norm(r);
  }
  /// acc += k * l
  void addScaled(Linear &acc, const Linear &l, const mpz_class &k) const {
    mpz_class t;
    mpz_addmul(t.get_mpz_t(), l.c.get_mpz_t(), k.get_mpz_t());
    mpz_add(t.get_mpz_t(), t.get_mpz_t(), acc.c.get_mpz_t());
    acc.c = norm(t);
    for (const auto &[a, v] : l.coef) {
      mpz_class s;
      mpz_mul(s.get_mpz_t(), v.get_mpz_t(), k.get_mpz_t());
      auto [p, fresh] = acc.coef.emplace(a, mpz_class(0UL));
      mpz_add(s.get_mpz_t(), s.get_mpz_t(), p->second.get_mpz_t());
      p->second = norm(s);
      if (p->second == mpz_class(0UL))
        acc.coef.erase(p);
    }
  }
  Linear atom(ENode *n) const {
    Linear l;
    l.coef.emplace(n, mpz_class(1UL));
    return l;
  }
  /// The symbolic part as a term (0 if empty).
  Expr rebuild(const Linear &l, ExprFactory &ef) {
    std::vector<Expr> terms;
    for (const auto &[a, v] : l.coef)
      terms.push_back(v == mpz_class(1UL)
                          ? Expr(a)
                          : mk<BMUL>(op::bv::bvnum(v, m_w, ef), Expr(a)));
    Expr r = terms.empty()       ? op::bv::bvnum(mpz_class(0UL), m_w, ef)
             : terms.size() == 1 ? terms[0]
                                 : mknary<BADD>(terms.begin(), terms.end());
    m_keep.push_back(r);
    return r;
  }
  Linear compute(ENode *n) {
    Expr e(n);
    if (m_values) {
      auto it = m_values->find(n);
      if (it != m_values->end()) {
        Linear l;
        l.c = norm(it->second);
        return l;
      }
    }
    if (op::bv::is_bvnum(e)) {
      Linear l;
      l.c = norm(op::bv::toMpz(e));
      return l;
    }
    const mpz_class one(1UL);
    mpz_class minusOne;
    mpz_sub_ui(minusOne.get_mpz_t(), m_mod.get_mpz_t(), 1);
    if (isOpX<BADD>(n) || isOpX<BSUB>(n)) {
      Linear l;
      for (size_t i = 0; i < n->arity(); ++i) {
        const Linear &k = of(n->arg(i));
        addScaled(l, k, (isOpX<BSUB>(n) && i > 0) ? minusOne : one);
      }
      return l;
    }
    if (isOpX<BNEG>(n) && n->arity() == 1) {
      Linear l;
      addScaled(l, of(n->arg(0)), minusOne);
      return l;
    }
    if (isOpX<BMUL>(n)) {
      mpz_class k(1UL);
      ENode *other = nullptr;
      size_t others = 0;
      for (size_t i = 0; i < n->arity(); ++i) {
        Expr kid(n->arg(i));
        if (op::bv::is_bvnum(kid))
          k = mul(k, op::bv::toMpz(kid));
        else {
          other = n->arg(i);
          ++others;
        }
      }
      Linear l;
      if (others == 0)
        l.c = k;
      else if (others == 1)
        addScaled(l, of(other), k);
      else
        return atom(n);
      return l;
    }
    if (isOpX<ITE>(n) && n->arity() == 3) {
      Linear t = of(n->arg(1)), f = of(n->arg(2));
      if (t.c != f.c)
        return atom(n);
      if (t.coef == f.coef)
        return t; // ite(c, x, x) = x
      Expr ite = mk<ITE>(Expr(n->arg(0)), rebuild(t, n->efac()),
                         rebuild(f, n->efac()));
      m_keep.push_back(ite);
      Linear l = atom(ite.get());
      l.c = t.c;
      return l;
    }
    return atom(n);
  }

  unsigned m_w;
  mpz_class m_mod;
  const std::unordered_map<ENode *, mpz_class> *m_values = nullptr;
  std::unordered_map<ENode *, Linear> m_memo;
  std::vector<Expr> m_keep;
};
} // namespace ackermann_detail
} // namespace expr
