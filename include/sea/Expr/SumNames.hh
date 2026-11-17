// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Named address sums (sea-vc --vc-defs; docs/design/SumNames.md).
//
// An encoding step of the solver-facing query, after the solver boundary and
// after the Ackermann lowering: not a program change, not a SeaM rewrite.
// The boundary erases every address to one flat n-ary bit-vector sum (SeaM's
// normalized sum, PtrSort.md "The boundary"); a parser cursor, a straight
// chain of adds, then gives sums of many summands that share their prefixes.
// A solver comparing two of them (an Ackermann pair) cancels the common
// summands and builds the residual sum of the pair: adders per pair, not per
// chain step.
//
// The step gives the shared sums names:
//  - a sum: an n-ary bvadd of the query with at least two summands that are
//    not numerals, as a unit (w, M, c): M the multiset of those summands, c
//    the numeral part modulo 2^w (two nodes of one unit are one sum);
//  - an index sum: a sum at an array index position (of a select or a store
//    of the query, of an Ackermann read), through ite leaves;
//  - the parent of a sum (M, c): the first collected sum with the same M and
//    another c, if this one is not that first one; else the first collected
//    sum of the largest multiset of the query's sums that is a proper
//    sub-multiset of M. Parents are strictly smaller (by multiset size, or the
//    first of one multiset), so they form a forest;
//  - the named sums: every index sum and every ancestor of one, except an
//    index sum with no parent and no child (it shares nothing);
//  - a name def.<k> (w bits; not with Nest) is defined as def.P + (M - P) + (c
//  - c_P) for a
//    parent P, else as the sum itself; every named sum is replaced by its
//    name everywhere in the query (the definitions too); other terms stay.
// The query becomes query' && defs: equisatisfiable (each name is a fresh
// constant fixed by its definition, so a model of the original constants
// extends to one of the names, uniquely), and every model of it is one of
// the query over the original constants. Nothing reads the names back:
// replay and model evaluation see the original terms.
//
// The form (DefForm):
//  - Nest: no name and no definition: every named sum is replaced by its
//    definition term (P's term + (M - P) + (c - c_P)), one node shared by
//    all its uses, so the query is a DAG of prefix sums instead of flat ones
//    (the emission form; solver-neutral, nothing asserted). A solver that
//    flattens nested sums (z3's simplifier, flat / som) gets the flat sums
//    back;
//  - Eq: (= def.k t), the plain equation;
//  - Hidden: (bvule def.k t) && (bvule t def.k), the same relation (bvule
//    is antisymmetric) as no top-level equation: a solver's variable
//    elimination (z3's solve-eqs) substitutes an equation back and gives the
//    flat sums again; this form survives it.
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/SmallArith.hh"
#include "sea/Expr/TypeChecker.hh"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace expr {

class SumNamer {
public:
  enum class DefForm { Nest, Eq, Hidden };

  struct Stats {
    uint64_t sums = 0;      ///< distinct sums of the query (units)
    uint64_t indexSums = 0; ///< of them at an index position
    uint64_t names = 0;     ///< named sums (fresh constants)
    uint64_t atoms = 0;     ///< definition atoms asserted (2 per Hidden name)
    uint64_t rewritten = 0; ///< bvadd nodes replaced by a name
  };

  explicit SumNamer(DefForm form, std::string prefix = "def.")
      : m_form(form), m_prefix(std::move(prefix)) {}

  /// An index term (an Ackermann read's index): its sums are index sums.
  /// The select / store indices of the query are added by name().
  void addIndex(const Expr &index) { m_roots.push_back(index); }

  /// \p query with the shared sums named and the definitions conjoined.
  Expr name(const Expr &query) {
    collect(query);
    for (const Expr &r : m_roots)
      markIndexSums(r);
    m_stats.sums = m_units.size();
    findParents();
    chooseNames();
    if (m_named.empty())
      return query;
    if (m_form == DefForm::Nest)
      return rewrite(query);
    std::vector<Expr> conj{rewrite(query)};
    for (size_t u : m_named) {
      Expr t = definition(u);
      Expr n = m_units[u].name;
      if (m_form == DefForm::Eq) {
        conj.push_back(mk<EQ>(n, t));
        ++m_stats.atoms;
      } else {
        conj.push_back(mk<BULE>(n, t));
        conj.push_back(mk<BULE>(t, n));
        m_stats.atoms += 2;
      }
    }
    return mknary<AND>(conj.begin(), conj.end());
  }

  const Stats &stats() const { return m_stats; }

private:
  static constexpr size_t npos = size_t(-1);
  /// a summand multiset: (summand, multiplicity), sorted by node id
  using Terms = std::vector<std::pair<ENode *, unsigned>>;
  struct Set {
    Terms terms;
    unsigned w = 0;
    size_t total = 0;  ///< sum of multiplicities
    size_t rep = npos; ///< the first unit with this multiset
  };
  struct Unit {
    size_t set = npos;
    mpz_class c;
    size_t parent = npos;
    bool index = false; ///< an index sum
    bool child = false; ///< some unit has it as parent
    bool named = false;
    Expr name; ///< the constant; Nest: the shared definition term (lazy)
  };

  struct IdLess {
    bool operator()(const ENode *a, const ENode *b) const {
      return a->getId() < b->getId();
    }
  };

  ExprFactory &efac() const { return *m_efac; }

  /// every sum of \p q, in a deterministic (DAG walk) order; the index roots
  /// of its selects and stores
  void collect(const Expr &q) {
    m_efac = &q->efac();
    std::unordered_set<ENode *> seen;
    std::vector<ENode *> stack{q.get()};
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      if ((isOpX<SELECT>(n) || isOpX<STORE>(n)) && n->arity() >= 2)
        m_roots.push_back(Expr(n->arg(1)));
      if (isOpX<BADD>(n))
        addSum(n);
      for (size_t i = n->arity(); i-- > 0;)
        stack.push_back(n->arg(i));
    }
  }

  void addSum(ENode *n) {
    std::map<ENode *, unsigned, IdLess> count;
    mpz_class c = 0L;
    for (size_t i = 0; i < n->arity(); ++i) {
      Expr a(n->arg(i));
      if (op::bv::isBvNum(a))
        smallarith::addTo(c, op::bv::toMpz(a));
      else
        ++count[n->arg(i)];
    }
    Terms t(count.begin(), count.end());
    size_t total = 0;
    for (auto &p : t)
      total += p.second;
    if (total < 2)
      return;
    unsigned w = op::bv::width(m_tc.typeOf(Expr(n)));
    c = smallarith::modPow2(c, w);
    auto skey = std::make_pair(w, t);
    auto sit = m_setIndex.find(skey);
    size_t s;
    if (sit == m_setIndex.end()) {
      s = m_sets.size();
      Set fresh;
      fresh.terms = std::move(t);
      fresh.w = w;
      fresh.total = total;
      m_sets.push_back(std::move(fresh));
      m_setIndex.emplace(std::move(skey), s);
      for (auto &p : m_sets[s].terms)
        m_bySummand[p.first].push_back(s);
    } else {
      s = sit->second;
    }
    auto ukey = std::make_pair(s, c);
    auto uit = m_unitIndex.find(ukey);
    size_t u;
    if (uit == m_unitIndex.end()) {
      u = m_units.size();
      Unit fresh;
      fresh.set = s;
      fresh.c = c;
      m_units.push_back(std::move(fresh));
      m_unitIndex.emplace(std::move(ukey), u);
      if (m_sets[s].rep == npos)
        m_sets[s].rep = u;
    } else {
      u = uit->second;
    }
    m_pin.push_back(Expr(n));
    m_nodeUnit.emplace(n, u);
  }

  /// the sums of \p e through ite leaves are index sums
  void markIndexSums(const Expr &e) {
    std::vector<ENode *> stack{e.get()};
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!m_indexSeen.insert(n).second)
        continue;
      if (isOpX<ITE>(n)) {
        stack.push_back(n->arg(1));
        stack.push_back(n->arg(2));
        continue;
      }
      auto it = m_nodeUnit.find(n);
      if (it != m_nodeUnit.end())
        m_units[it->second].index = true;
    }
  }

  static unsigned multOf(const Terms &t, ENode *s) {
    auto it = std::lower_bound(
        t.begin(), t.end(), s,
        [](const std::pair<ENode *, unsigned> &p, const ENode *x) {
          return p.first->getId() < x->getId();
        });
    return it != t.end() && it->first == s ? it->second : 0;
  }

  /// the largest proper sub-multiset of set \p m among the sets (ties: the
  /// first collected); npos if none
  size_t largestSubset(size_t m) {
    const Set &sm = m_sets[m];
    std::unordered_map<size_t, unsigned> hits;
    for (auto &[s, k] : sm.terms)
      for (size_t t : m_bySummand[s])
        if (t != m && m_sets[t].w == sm.w && m_sets[t].total < sm.total &&
            multOf(m_sets[t].terms, s) <= k)
          ++hits[t];
    size_t best = npos;
    for (auto &[t, h] : hits) {
      if (h != m_sets[t].terms.size())
        continue;
      if (best == npos || m_sets[t].total > m_sets[best].total ||
          (m_sets[t].total == m_sets[best].total && t < best))
        best = t;
    }
    return best;
  }

  /// parents of the index sums and of their ancestors only
  void findParents() {
    std::vector<size_t> work;
    std::vector<bool> done(m_units.size(), false);
    for (size_t u = 0; u < m_units.size(); ++u)
      if (m_units[u].index) {
        ++m_stats.indexSums;
        work.push_back(u);
      }
    std::unordered_map<size_t, size_t> subsetMemo;
    while (!work.empty()) {
      size_t u = work.back();
      work.pop_back();
      if (done[u])
        continue;
      done[u] = true;
      Unit &un = m_units[u];
      size_t rep = m_sets[un.set].rep;
      size_t p = npos;
      if (rep != u) {
        p = rep;
      } else {
        auto it = subsetMemo.find(un.set);
        size_t sub = it != subsetMemo.end()
                         ? it->second
                         : (subsetMemo[un.set] = largestSubset(un.set));
        if (sub != npos)
          p = m_sets[sub].rep;
      }
      if (p == npos)
        continue;
      un.parent = p;
      m_units[p].child = true;
      if (!done[p])
        work.push_back(p);
    }
    m_onChain = std::move(done);
  }

  void chooseNames() {
    for (size_t u = 0; u < m_units.size(); ++u) {
      Unit &un = m_units[u];
      if (!m_onChain[u])
        continue;
      if (un.parent == npos && !un.child)
        continue; // an index sum that shares nothing
      un.named = true;
      if (m_form != DefForm::Nest) {
        std::string nm = m_prefix + std::to_string(m_named.size());
        un.name =
            op::bv::bvConst(mkTerm<std::string>(nm, efac()), m_sets[un.set].w);
      }
      m_named.push_back(u);
      ++m_stats.names;
    }
  }

  Expr num(const mpz_class &v, unsigned w) {
    return op::bv::bvnum(smallarith::modPow2(v, w), w, efac());
  }

  /// what a named sum becomes: its name, or (Nest) its definition term, one
  /// node shared by every use
  Expr form(size_t u) {
    if (!m_units[u].name)
      m_units[u].name = definition(u);
    return m_units[u].name;
  }

  /// def.P + (M - P) + (c - c_P), or the sum itself (summands rewritten)
  Expr definition(size_t u) {
    const Unit &un = m_units[u];
    const Set &s = m_sets[un.set];
    std::vector<Expr> args;
    mpz_class c = un.c;
    const Set *ps = nullptr;
    if (un.parent != npos) {
      const Unit &p = m_units[un.parent];
      args.push_back(form(un.parent));
      ps = &m_sets[p.set];
      c = smallarith::modPow2Sub(un.c, p.c, s.w);
    }
    for (auto &[t, k] : s.terms) {
      unsigned drop = ps ? multOf(ps->terms, t) : 0;
      for (unsigned i = drop; i < k; ++i)
        args.push_back(rewrite(Expr(t)));
    }
    c = smallarith::modPow2(c, s.w);
    if (c != 0L)
      args.push_back(num(c, s.w));
    return args.size() == 1 ? args[0] : mknary<BADD>(args.begin(), args.end());
  }

  /// \p root with every named sum replaced by its name (memoized)
  Expr rewrite(const Expr &root) {
    std::vector<std::pair<ENode *, bool>> stack{{root.get(), false}};
    while (!stack.empty()) {
      auto [n, done] = stack.back();
      if (m_memo.count(n)) {
        stack.pop_back();
        continue;
      }
      auto it = m_nodeUnit.find(n);
      if (it != m_nodeUnit.end() && m_units[it->second].named) {
        Expr f = form(it->second);
        stack.pop_back();
        ++m_stats.rewritten;
        m_memo.emplace(n, f);
        continue;
      }
      if (!done) {
        stack.back().second = true;
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_memo.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      stack.pop_back();
      ExprVector kids;
      bool changed = false;
      for (size_t i = 0; i < n->arity(); ++i) {
        Expr k = m_memo.at(n->arg(i));
        changed |= k.get() != n->arg(i);
        kids.push_back(k);
      }
      Expr res = changed ? n->efac().mkNary(n->op(), kids) : Expr(n);
      m_pin.push_back(Expr(n));
      m_memo.emplace(n, res);
    }
    return m_memo.at(root.get());
  }

  DefForm m_form;
  std::string m_prefix;
  ExprFactory *m_efac = nullptr;
  TypeChecker m_tc;
  std::vector<Expr> m_roots;
  std::unordered_set<ENode *> m_indexSeen;
  std::vector<Set> m_sets;
  std::map<std::pair<unsigned, Terms>, size_t> m_setIndex;
  std::unordered_map<ENode *, std::vector<size_t>> m_bySummand;
  std::vector<Unit> m_units;
  std::map<std::pair<size_t, mpz_class>, size_t> m_unitIndex;
  std::unordered_map<ENode *, size_t> m_nodeUnit;
  std::vector<bool> m_onChain;
  std::vector<size_t> m_named;
  std::unordered_map<ENode *, Expr> m_memo;
  std::vector<Expr> m_pin;
  Stats m_stats;
};

} // namespace expr
