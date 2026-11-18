// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The difference decider (docs/design/Ackermann.md, "The difference
// decider"): are two w-bit terms a and b never equal, always equal, or
// neither, for every assignment? Exact: NEVER means a != b in every model
// (of the query, with premise LAYOUT: the known values of the object
// constants, conjuncts of the query); no other assumption.
//
// It works on the cancelled difference D = a - b as a linear form over w
// bits (LinearForms: D = sum s_i x_i + c mod 2^w, s_i the coefficients, x_i
// the atoms) with each atom's integer representative ranging over an
// interval (BvRanges). Then D as an integer lies in [lo, hi] with D = c'
// (mod g), g = gcd(s_i), c' the representative of c, and a = b iff D = 0
// (mod 2^w):
//   - residue: no atom (D = c, c != 0), or c not a multiple of
//     gcd(g, 2^w) (every coefficient a multiple of 2^t, c not);
//   - interval: no multiple of 2^w in [lo, hi];
//   - congruence: no multiple m of 2^w in [lo, hi] with m = c' (mod g),
//     where [lo, hi] holds some multiple (the interval was needed: without a
//     bounded interval the congruence decides exactly the residue pairs,
//     gcd(g, 2^w) being a power of two, and those count as residue);
//   - signed: the same two with the atoms' signed representatives
//     (sext(x) read as the signed x, negative numerals; BvRanges::repr)
//     instead of their unsigned values;
//   - ALWAYS: D is 0 as a form.
// The tests run on the unsplit form first, then leaf by leaf: each term as
// the set of linear forms of its leaves (through ite, bvadd and bvsub: every
// combination, each with the ite conditions it was taken under; a
// combination that takes one condition (the same node) both ways is
// infeasible and dropped). Leaf semantics: the unsplit pair is tested
// first; if it is not decided, the pair is NEVER iff every feasible pair
// (leaf of a, leaf of b) of the product is NEVER by the tests above on its
// own linear form (an ALWAYS leaf pair is a pair that may be equal). The
// representatives are free (any integer congruent modulo 2^w); this code
// takes the symmetric one for coefficients and the constant.
// Splitting is not monotone (an ite common to both terms cancels as an atom
// but splits into uncorrelated leaves), so both are tried. A node whose
// leaves would be more than Options::maxLeaves is one leaf (its linear
// form); a pair with more than Options::maxLeafPairs leaf pairs is not split
// (budget, counted).
//
// Each test runs first without the known values (premise none), then with
// them (premise LAYOUT: object constants replaced by their numerals).
// Callers run the decider on the terms the encoding compares, at their
// width: a NEVER at 64 bits says nothing about the low k bits of the same
// offsets (0 vs 2048 at k = 11).
#include "sea/Expr/BvRange.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/LinearForms.hh"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {

/// The leaf budgets of DiffDecider.
struct DiffDeciderOptions {
  /// at most this many leaves per node (else the node is one leaf); a first
  /// guess (untuned)
  size_t maxLeaves = 256;
  /// at most this many leaf pairs per term pair (else no split)
  size_t maxLeafPairs = 16384;
};

class DiffDecider {
public:
  enum class Verdict { Never, Always, Unknown };
  enum class Premise { None, Layout };
  /// Why a NEVER holds (the weakest test any leaf pair needed, in this order).
  enum class Class { None, Residue, Interval, Congruence, Signed };

  using Options = DiffDeciderOptions;

  struct Result {
    Verdict verdict = Verdict::Unknown;
    Premise premise = Premise::None;
    Class cls = Class::None;
    /// decided leaf by leaf (the unsplit form did not decide)
    bool split = false;
    /// a leaf budget was hit (a node made one leaf, or too many pairs)
    bool budget = false;
  };

  DiffDecider(unsigned w, const std::unordered_map<ENode *, mpz_class> *values,
              Options opt = Options())
      : m_w(w), m_opt(opt) {
    m_mod = bvz::pow2(w);
    m_passes.push_back(std::make_unique<Pass>(w, nullptr));
    if (values && !values->empty())
      m_passes.push_back(std::make_unique<Pass>(w, values));
  }

  unsigned width() const { return m_w; }

  Result decide(ENode *a, ENode *b) {
    Result res;
    // the unsplit forms
    for (size_t p = 0; p < m_passes.size(); ++p) {
      Pass &ps = *m_passes[p];
      ackermann_detail::Linear d = ps.lf.of(a);
      ps.lf.add(d, ps.lf.of(b), /*negate=*/true);
      Test t = test(ps, d);
      if (t.verdict != Verdict::Unknown) {
        res.verdict = t.verdict;
        res.cls = t.cls;
        res.premise = p ? Premise::Layout : Premise::None;
        return res;
      }
    }
    // leaf by leaf
    for (size_t p = 0; p < m_passes.size(); ++p) {
      Pass &ps = *m_passes[p];
      const LeafSet &la = leaves(ps, a);
      const LeafSet &lb = leaves(ps, b);
      res.budget = res.budget || la.truncated || lb.truncated;
      if (la.leaves.size() == 1 && lb.leaves.size() == 1)
        continue; // the unsplit forms, tested above
      if (la.leaves.size() * lb.leaves.size() > m_opt.maxLeafPairs) {
        res.budget = true;
        continue;
      }
      Class worst = Class::None;
      bool never = true;
      for (const Leaf &x : la.leaves) {
        for (const Leaf &y : lb.leaves) {
          Test t = leafPair(ps, x.id, y.id);
          if (t.verdict == Verdict::Never) {
            worst = std::max(worst, t.cls);
            continue;
          }
          if (!compatible(x.conds, y.conds))
            continue; // infeasible: the pair does not occur
          never = false;
          break;
        }
        if (!never)
          break;
      }
      if (never) {
        res.verdict = Verdict::Never;
        res.cls = worst == Class::None ? Class::Residue : worst;
        res.premise = p ? Premise::Layout : Premise::None;
        res.split = true;
        return res;
      }
    }
    return res;
  }

private:
  using Linear = ackermann_detail::Linear;
  using LinearForms = ackermann_detail::LinearForms;
  /// (condition, taken as true)
  using Cond = std::pair<ENode *, bool>;
  using Conds = llvm::SmallVector<Cond, 2>; ///< sorted by node

  struct Leaf {
    uint32_t id; ///< interned linear form (Pass::forms)
    Conds conds;
  };
  struct LeafSet {
    std::vector<Leaf> leaves;
    bool truncated = false;
  };
  struct Test {
    Verdict verdict = Verdict::Unknown;
    Class cls = Class::None;
  };
  /// The state of one premise level: linear forms, ranges, leaves, leaf-pair
  /// answers.
  struct Pass {
    Pass(unsigned w, const std::unordered_map<ENode *, mpz_class> *values)
        : lf(w), ranges(values) {
      lf.setValues(values);
    }
    LinearForms lf;
    BvRanges ranges;
    std::unordered_map<ENode *, LeafSet> leafMemo;
    std::vector<Expr> keep; ///< keeps the leafMemo keys alive
    std::map<std::pair<LinearForms::Key, mpz_class>, uint32_t> ids;
    std::vector<Linear> forms;
    std::unordered_map<uint64_t, Test> pairMemo;
  };

  uint32_t intern(Pass &ps, Linear l) {
    auto key = std::make_pair(ps.lf.key(l), l.c);
    auto it = ps.ids.find(key);
    if (it != ps.ids.end())
      return it->second;
    uint32_t id = uint32_t(ps.forms.size());
    ps.forms.push_back(std::move(l));
    ps.ids.emplace(std::move(key), id);
    return id;
  }

  /// Merge two sorted condition sets; false if one condition is taken both
  /// ways.
  static bool merge(const Conds &x, const Conds &y, Conds &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < x.size() || j < y.size()) {
      if (j == y.size() || (i < x.size() && x[i].first < y[j].first)) {
        out.push_back(x[i++]);
      } else if (i == x.size() || y[j].first < x[i].first) {
        out.push_back(y[j++]);
      } else {
        if (x[i].second != y[j].second)
          return false;
        out.push_back(x[i]);
        ++i;
        ++j;
      }
    }
    return true;
  }
  static bool compatible(const Conds &x, const Conds &y) {
    size_t i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
      if (x[i].first < y[j].first)
        ++i;
      else if (y[j].first < x[i].first)
        ++j;
      else {
        if (x[i].second != y[j].second)
          return false;
        ++i;
        ++j;
      }
    }
    return true;
  }

  /// bvadd (any arity) or a binary bvsub; other bvsub arities are atoms
  static bool isSplitSum(ENode *n) {
    return (isOpX<BADD>(n) && n->arity() >= 1) ||
           (isOpX<BSUB>(n) && n->arity() == 2);
  }

  const LeafSet &leaves(Pass &ps, ENode *n) {
    auto it = ps.leafMemo.find(n);
    if (it != ps.leafMemo.end())
      return it->second;
    LeafSet res;
    bool split = false;
    if (isOpX<ITE>(n) && n->arity() == 3) {
      split = true;
      ENode *c = n->arg(0);
      for (int br = 1; br <= 2 && split; ++br) {
        const LeafSet &s = leaves(ps, n->arg(br));
        res.truncated = res.truncated || s.truncated;
        Conds one{Cond(c, br == 1)}, m;
        for (const Leaf &l : s.leaves) {
          if (!merge(l.conds, one, m))
            continue;
          res.leaves.push_back(Leaf{l.id, m});
        }
        if (res.leaves.size() > m_opt.maxLeaves)
          split = false;
      }
    } else if (isSplitSum(n)) {
      split = true;
      const LeafSet &s0 = leaves(ps, n->arg(0));
      res.truncated = s0.truncated;
      res.leaves = s0.leaves;
      for (size_t i = 1; i < n->arity() && split; ++i) {
        const LeafSet &si = leaves(ps, n->arg(i));
        res.truncated = res.truncated || si.truncated;
        std::vector<Leaf> out;
        Conds m;
        for (size_t x = 0; x < res.leaves.size() && split; ++x)
          for (const Leaf &y : si.leaves) {
            if (!merge(res.leaves[x].conds, y.conds, m))
              continue;
            if (out.size() == m_opt.maxLeaves) {
              split = false;
              break;
            }
            Linear z = ps.forms[res.leaves[x].id];
            ps.lf.add(z, ps.forms[y.id], isOpX<BSUB>(n));
            out.push_back(Leaf{intern(ps, std::move(z)), m});
          }
        res.leaves = std::move(out);
      }
    }
    if (!split || res.leaves.empty()) {
      // a node that splits but has too many leaves: one leaf, its form
      bool splits = (isOpX<ITE>(n) && n->arity() == 3) || isSplitSum(n);
      res.truncated = res.truncated || (splits && !split);
      res.leaves.clear();
      res.leaves.push_back(Leaf{intern(ps, ps.lf.of(n)), {}});
    }
    ps.keep.push_back(Expr(n));
    return ps.leafMemo.emplace(n, std::move(res)).first->second;
  }

  Test leafPair(Pass &ps, uint32_t x, uint32_t y) {
    uint64_t key = (uint64_t(x) << 32) | y;
    auto it = ps.pairMemo.find(key);
    if (it != ps.pairMemo.end())
      return it->second;
    Linear d = ps.forms[x];
    ps.lf.add(d, ps.forms[y], /*negate=*/true);
    Test t = test(ps, d);
    if (t.verdict == Verdict::Always)
      t.verdict = Verdict::Unknown; // equal leaves: the pair may be equal
    ps.pairMemo.emplace(key, t);
    return t;
  }

  mpz_class symmetric(const mpz_class &v) const {
    return bvz::symmetric(v, m_w);
  }

  /// The tests on D = sum s_i x_i + c (see the header).
  Test test(Pass &ps, const Linear &d) {
    if (d.coef.empty())
      return {d.c == mpz_class(0L) ? Verdict::Always : Verdict::Never,
              Class::Residue};
    // residue: gcd(g, 2^w) = 2^t, t the least trailing zeros of the s_i
    unsigned t = m_w;
    for (const auto &[a, s] : d.coef)
      t = std::min(t, trailingZeros(s));
    if (trailingZeros(d.c) < t)
      return {Verdict::Never, Class::Residue};
    for (int sgn = 0; sgn < 2; ++sgn) {
      mpz_class lo = symmetric(d.c), hi = lo, g(0L);
      bool bounded = true, anySigned = false;
      for (const auto &[a, s0] : d.coef) {
        mpz_class s = symmetric(s0);
        mpz_gcd(g.get_mpz_t(), g.get_mpz_t(), s.get_mpz_t());
        BvInterval x =
            sgn ? ps.ranges.repr(Expr(a)) : ps.ranges.unsignedRange(Expr(a));
        if (!x.known) {
          bounded = false;
          break;
        }
        anySigned = anySigned || x.lo < 0L;
        const mpz_class &xl = s >= 0L ? x.lo : x.hi;
        const mpz_class &xh = s >= 0L ? x.hi : x.lo;
        lo = bvz::add(lo, bvz::mul(s, xl));
        hi = bvz::add(hi, bvz::mul(s, xh));
      }
      if (!bounded) {
        if (sgn)
          break;
        continue; // the signed pass may still decide
      }
      if (sgn && !anySigned)
        break; // the same intervals as the unsigned pass
      Class cls = noMultiple(lo, hi, symmetric(d.c), g);
      if (cls != Class::None)
        return {Verdict::Never, sgn ? Class::Signed : cls};
    }
    return {};
  }

  /// No multiple of 2^w in [lo, hi] (Interval), or none congruent to cc
  /// modulo g (Congruence); else None.
  Class noMultiple(const mpz_class &lo, const mpz_class &hi,
                   const mpz_class &cc, const mpz_class &g) const {
    mpz_class kmin, kmax;
    mpz_cdiv_q(kmin.get_mpz_t(), lo.get_mpz_t(), m_mod.get_mpz_t());
    mpz_fdiv_q(kmax.get_mpz_t(), hi.get_mpz_t(), m_mod.get_mpz_t());
    if (kmin > kmax)
      return Class::Interval;
    // k 2^w = cc (mod g): with e = gcd(2^w, g) | cc (the residue test), k =
    // k0 (mod g / e), k0 = (cc / e) * inverse(2^w / e) modulo g / e
    mpz_class e, ge, me, inv, k0, first;
    mpz_gcd(e.get_mpz_t(), m_mod.get_mpz_t(), g.get_mpz_t());
    if (!mpz_divisible_p(cc.get_mpz_t(), e.get_mpz_t()))
      return Class::Residue; // not reached: the residue test decides it
    ge = bvz::fdiv(g, e);
    if (ge <= 1L)
      return Class::None;
    me = bvz::fdiv(m_mod, e);
    if (!mpz_invert(inv.get_mpz_t(), me.get_mpz_t(), ge.get_mpz_t()))
      return Class::None; // not reached: coprime
    k0 = bvz::mul(bvz::fdiv(cc, e), inv);
    first = bvz::add(kmin, bvz::fmod(bvz::sub(k0, kmin), ge));
    return first > kmax ? Class::Congruence : Class::None;
  }

  unsigned trailingZeros(const mpz_class &x) const {
    mpz_class r = bvz::fmod(x, m_mod);
    if (r == mpz_class(0L))
      return m_w;
    return unsigned(mpz_scan1(r.get_mpz_t(), 0));
  }

  unsigned m_w;
  mpz_class m_mod;
  Options m_opt;
  std::vector<std::unique_ptr<Pass>> m_passes;
};

} // namespace expr
