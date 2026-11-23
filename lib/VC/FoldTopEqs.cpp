// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- FoldTopEqs.cpp - substitute the VC's top-level facts ---------------===//
//
// docs/design/FoldTopEqs.md
//
//===----------------------------------------------------------------------===//

#include "sea/VC/FoldTopEqs.h"

#include "sea/Expr/ExprOpArray.hh"
#include "sea/Expr/ExprOpBind.hh"
#include "sea/Expr/ExprOpBool.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/ExprOpPtr.hh"
#include "sea/Expr/TypeChecker.hh"

#include <algorithm>
#include <unordered_set>

using namespace expr;

namespace sea {

namespace {
/// the conjuncts of \p e (nested ANDs flattened), each once, in order
void conjuncts(const Expr &e, ExprVector &out) {
  std::unordered_set<ENode *> seen;
  std::vector<ENode *> stack{e.get()};
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (isOpX<AND>(n)) {
      for (size_t i = n->arity(); i-- > 0;)
        stack.push_back(n->arg(i));
      continue;
    }
    if (isOpX<TRUE>(n))
      continue;
    if (seen.insert(n).second)
      out.push_back(Expr(n));
  }
}

bool isNumeral(const Expr &e) { return op::bv::isBvNum(e); }

using Map = std::unordered_map<ENode *, std::pair<Expr, Expr>>;

/// \p e with every key of \p m replaced, memoized in \p cache; \p uses
/// (optional) counts the (parent, operand) pairs replaced
Expr substMap(const Expr &e, const Map &m, Map &cache, uint64_t *uses) {
  if (auto it = m.find(e.get()); it != m.end())
    return it->second.second;
  // post-order, explicit stack (VC terms are deep)
  std::vector<std::pair<ENode *, bool>> stack{{e.get(), false}};
  while (!stack.empty()) {
    auto [n, done] = stack.back();
    stack.pop_back();
    if (cache.count(n))
      continue;
    if (!done) {
      stack.push_back({n, true});
      for (size_t i = 0; i < n->arity(); ++i) {
        ENode *a = n->arg(i);
        if (!m.count(a) && !cache.count(a))
          stack.push_back({a, false});
      }
      continue;
    }
    bool changed = false;
    llvm::SmallVector<Expr, 4> kids;
    kids.reserve(n->arity());
    for (size_t i = 0; i < n->arity(); ++i) {
      ENode *a = n->arg(i);
      Expr k;
      if (auto it = m.find(a); it != m.end()) {
        k = it->second.second;
        if (uses)
          ++*uses;
      } else {
        k = cache.at(a).second;
      }
      changed |= k.get() != a;
      kids.push_back(k);
    }
    Expr r =
        changed ? n->efac().mkNary(n->op(), kids.begin(), kids.end()) : Expr(n);
    cache.emplace(n, std::make_pair(Expr(n), r));
  }
  return cache.at(e.get()).second;
}

/// The concat-ite hoist (ConcatIte), memoized per concat node over one
/// hoistConcatItes call. A memo key is held: the concat of the arms is made
/// here and a freed node's address may be reused by another node.
class ConcatIteHoister {
public:
  ConcatIteHoister(ConcatIte mode, FoldTopEqsStats &st) : mode(mode), st(st) {}

  /// the hoisted form of the concat \p t (t itself when the hoist does not
  /// apply or the mode refuses it)
  Expr top(const Expr &t) {
    if (!condOf(t))
      return t;
    // the ite nodes the parts hold along their ite spines: the bound on the
    // ites a Numeral hoist may make (it does not grow the term)
    budget = UINT64_MAX;
    if (mode == ConcatIte::Numeral) {
      std::unordered_set<ENode *> seen;
      std::vector<ENode *> stack(t->args_begin(), t->args_end());
      while (!stack.empty()) {
        ENode *n = stack.back();
        stack.pop_back();
        if (!isOpX<ITE>(n) || !seen.insert(n).second)
          continue;
        stack.push_back(n->arg(1));
        stack.push_back(n->arg(2));
      }
      budget = seen.size();
    }
    made = kept = 0;
    added.clear();
    Expr r = hoist(t);
    if (!r) {
      // the concats this top tried are not counted: a later top counts them
      // with its own budget (the count does not depend on the visit order)
      for (ENode *n : added)
        memo.erase(n);
      ++st.hoistRefused;
      return t;
    }
    st.hoisted += kept;
    return r;
  }

private:
  /// the condition c when every part of the concat \p t is a numeral or an
  /// ite on c, and one is an ite; else null (\p other: a part is neither)
  static Expr condOf(const Expr &t, bool *other = nullptr) {
    Expr c;
    for (size_t i = 0; i < t->arity(); ++i) {
      ENode *p = t->arg(i);
      if (isNumeral(Expr(p)))
        continue;
      if (!isOpX<ITE>(p) || (c && p->arg(0) != c.get())) {
        if (other)
          *other = true;
        return Expr();
      }
      c = Expr(p->arg(0));
    }
    return c;
  }

  /// \p t hoisted; null: refused (Numeral: a leaf not a concat of
  /// numerals, or over the budget)
  Expr hoist(const Expr &t) {
    if (auto it = memo.find(t.get()); it != memo.end())
      return it->second.second;
    bool other = false;
    Expr c = condOf(t, &other);
    Expr res;
    if (!c)
      // a leaf: numerals only, or a part neither a numeral nor an ite on
      // the condition of the others (Numeral refuses it)
      res = other && mode == ConcatIte::Numeral ? Expr() : t;
    else if (++made <= budget) {
      llvm::SmallVector<Expr, 4> th, el;
      for (size_t i = 0; i < t->arity(); ++i) {
        ENode *p = t->arg(i);
        th.push_back(isOpX<ITE>(p) ? Expr(p->arg(1)) : Expr(p));
        el.push_back(isOpX<ITE>(p) ? Expr(p->arg(2)) : Expr(p));
      }
      ExprFactory &efac = t->efac();
      Expr a = hoist(efac.mkNary(t->op(), th.begin(), th.end()));
      Expr b = a ? hoist(efac.mkNary(t->op(), el.begin(), el.end())) : Expr();
      if (a && b) {
        ++kept;
        res = mk<ITE>(c, a, b);
      }
    }
    // a refusal over the budget is not memoized: another top concat has
    // its own budget
    if (res || made <= budget)
      if (memo.emplace(t.get(), std::make_pair(t, res)).second)
        added.push_back(t.get());
    return res;
  }

  ConcatIte mode;
  FoldTopEqsStats &st;
  Map memo;
  /// the memo keys added since the current top concat began
  std::vector<ENode *> added;
  /// the current top concat: its bound, the concats it tried, those it
  /// made ites (counted when it is kept)
  uint64_t budget = 0, made = 0, kept = 0;
};
} // namespace

Expr hoistConcatItes(const Expr &e, ConcatIte mode, FoldTopEqsStats &st) {
  if (mode == ConcatIte::Off)
    return e;
  ConcatIteHoister ch(mode, st);
  Map cache;
  std::vector<std::pair<ENode *, bool>> stack{{e.get(), false}};
  while (!stack.empty()) {
    auto [n, done] = stack.back();
    stack.pop_back();
    if (cache.count(n))
      continue;
    if (!done) {
      stack.push_back({n, true});
      for (size_t i = 0; i < n->arity(); ++i)
        if (!cache.count(n->arg(i)))
          stack.push_back({n->arg(i), false});
      continue;
    }
    bool changed = false;
    llvm::SmallVector<Expr, 4> kids;
    for (size_t i = 0; i < n->arity(); ++i) {
      kids.push_back(cache.at(n->arg(i)).second);
      changed |= kids.back().get() != n->arg(i);
    }
    Expr r =
        changed ? n->efac().mkNary(n->op(), kids.begin(), kids.end()) : Expr(n);
    if (isOpX<BCONCAT>(r))
      r = ch.top(r);
    cache.emplace(n, std::make_pair(Expr(n), r));
  }
  return cache.at(e.get()).second;
}

namespace {
bool isVar(const Expr &e) {
  return bind::isBoolConst(e) || op::bv::isBvConst(e);
}

/// \p t = \p c (a numeral of t's width) as definitions of constants: t a
/// constant, a numeral equal to c, or a concat of such parts
bool constParts(const Expr &t, const Expr &c,
                std::vector<std::pair<Expr, Expr>> &out) {
  if (op::bv::isBvConst(t)) {
    out.emplace_back(t, c);
    return true;
  }
  if (isNumeral(t))
    return op::bv::toMpz(t) == op::bv::toMpz(c);
  if (!isOpX<BCONCAT>(t))
    return false;
  std::vector<unsigned> widths;
  for (size_t i = 0; i < t->arity(); ++i) {
    unsigned w = op::bv::knownWidth(Expr(t->arg(i)));
    if (!w)
      return false;
    widths.push_back(w);
  }
  mpz_class v = op::bv::toMpz(c);
  unsigned low = 0;
  for (size_t i = t->arity(); i-- > 0;) {
    unsigned w = widths[i];
    mpz_class part = (v >> low) & ((mpz_class(1UL) << w) - 1);
    low += w;
    if (!constParts(Expr(t->arg(i)), op::bv::bvnum(part, w, t->efac()), out))
      return false;
  }
  return true;
}

/// the definitions conjunct \p c makes, if it is one
bool definitions(const Expr &c, std::vector<std::pair<Expr, Expr>> &out) {
  if (bind::isBoolConst(c)) {
    out.emplace_back(c, mk<TRUE>(c->efac()));
    return true;
  }
  if (isOpX<NEG>(c) && c->arity() == 1 && bind::isBoolConst(Expr(c->arg(0)))) {
    out.emplace_back(Expr(c->arg(0)), mk<FALSE>(c->efac()));
    return true;
  }
  if (!isOpX<EQ>(c) || c->arity() != 2)
    return false;
  Expr t = c->arg(0), v = c->arg(1);
  if (isNumeral(t))
    std::swap(t, v);
  if (!isNumeral(v) || isNumeral(t))
    return false;
  return constParts(t, v, out);
}
} // namespace

Expr TopConstEliminator::eliminate(Expr query) {
  std::vector<std::pair<Expr, Expr>> ds;
  for (;;) {
    ExprVector conj, kept;
    conjuncts(query, conj);
    bool any = false;
    for (const Expr &c : conj) {
      ds.clear();
      // a definition of a constant already defined as another value is
      // not one (substituted: false)
      bool def = definitions(c, ds);
      for (size_t i = 0; def && i < ds.size(); ++i) {
        auto it = defs.find(ds[i].first.get());
        def = it == defs.end() || it->second.second == ds[i].second;
        for (size_t j = 0; def && j < i; ++j)
          def = ds[j].first != ds[i].first || ds[j].second == ds[i].second;
      }
      if (!def) {
        kept.push_back(c);
        continue;
      }
      for (const auto &[x, v] : ds)
        if (defs.emplace(x.get(), std::make_pair(x, v)).second) {
          entries.push_back({x, v});
          any = true;
        }
    }
    cache.clear();
    for (Expr &c : kept)
      c = substMap(c, defs, cache, nullptr);
    query = boolop::land(kept, query->efac());
    if (!any)
      break;
    ++nRounds;
  }
  cache.clear();
  return query;
}

Expr TopConstEliminator::image(const Expr &e) {
  return defs.empty() ? e : substMap(e, defs, cache, nullptr);
}

bool TopEqFolder::mentionsObject(const Expr &e) {
  if (!baseSkip)
    return false;
  // post-order over the DAG, reads and memories not entered; memoized
  // across facts and rounds (the key held: a freed node's address can come
  // back)
  std::vector<std::pair<ENode *, bool>> stack{{e.get(), false}};
  while (!stack.empty()) {
    auto [n, expanded] = stack.back();
    stack.pop_back();
    if (objMemo.count(n))
      continue;
    if (op::ptr::isSegment(n) || isOpX<SELECT>(n) || isOpX<W_SELECT>(n) ||
        isOpX<STORE>(n) || isOpX<W_STORE>(n) || n->arity() == 0) {
      objMemo.emplace(n, std::make_pair(Expr(n), op::ptr::isSegment(n)));
      continue;
    }
    if (!expanded) {
      stack.push_back({n, true});
      for (size_t i = 0; i < n->arity(); ++i)
        if (!objMemo.count(n->arg(i)))
          stack.push_back({n->arg(i), false});
      continue;
    }
    bool d = false;
    for (size_t i = 0; i < n->arity() && !d; ++i)
      d = objMemo.at(n->arg(i)).second;
    objMemo.emplace(n, std::make_pair(Expr(n), d));
  }
  return objMemo.at(e.get()).second;
}

void TopEqFolder::splitConcat(const Expr &t, const Expr &c,
                              std::vector<std::tuple<Expr, Expr, Kind>> &out) {
  // parts from the high end; each part's width by sort inference
  TypeChecker tc;
  std::vector<unsigned> widths;
  for (size_t i = 0; i < t->arity(); ++i) {
    Expr s = tc.typeOf(t->arg(i));
    if (!s || !isOpX<BVSORT>(s)) {
      ++st.skippedUntyped;
      return;
    }
    widths.push_back(op::bv::width(s));
  }
  mpz_class v = op::bv::toMpz(c);
  unsigned low = 0;
  for (size_t i = t->arity(); i-- > 0;) {
    unsigned w = widths[i];
    mpz_class part = (v >> low) & ((mpz_class(1UL) << w) - 1);
    low += w;
    const Expr &a = t->arg(i);
    if (isNumeral(a))
      continue;
    Expr pc = op::bv::bvnum(part, w, t->efac());
    if (mentionsObject(a))
      ++st.skippedBase;
    else
      out.emplace_back(a, pc, Kind::Split);
    if (isOpX<BCONCAT>(a))
      splitConcat(a, pc, out);
  }
}

void TopEqFolder::factsOf(const Expr &c,
                          std::vector<std::tuple<Expr, Expr, Kind>> &out) {
  if (isOpX<EQ>(c) && c->arity() == 2) {
    Expr t = c->arg(0), v = c->arg(1);
    if (isNumeral(t))
      std::swap(t, v);
    if (isNumeral(v) && !isNumeral(t)) {
      // bvand(x, m) = v fixes the bits of x under m only
      if (isOpX<BAND>(t) &&
          std::any_of(t->args_begin(), t->args_end(),
                      [](ENode *a) { return isNumeral(a); })) {
        ++st.skippedMasked;
        return;
      }
      // a term that mentions an object (an address in it, an extract or a
      // sum of one, a pointer difference with it): not substituted
      // (mentionsObject); a concat of one still gives its other parts
      if (mentionsObject(t))
        ++st.skippedBase;
      else
        out.emplace_back(t, v, Kind::Term);
      if (isOpX<BCONCAT>(t))
        splitConcat(t, v, out);
      return;
    }
  }
  if (isOpX<NEG>(c) && c->arity() == 1 && bind::isBoolConst(Expr(c->arg(0)))) {
    out.emplace_back(Expr(c->arg(0)), mk<FALSE>(c->efac()), Kind::Bool);
    return;
  }
  if (bind::isBoolConst(c))
    out.emplace_back(c, mk<TRUE>(c->efac()), Kind::Bool);
}

Expr TopEqFolder::subst(const Expr &e, const Map &m, Cache &cache, bool count) {
  return substMap(e, m, cache, count ? &st.uses : nullptr);
}

Expr TopEqFolder::keep(const Expr &e, const std::vector<ENode *> &own,
                       const Map &m, Cache &cache, bool root) {
  // the defining conjunct and the path to its keys are rebuilt, not
  // replaced: (= T c) stays (= T' c), never (= c c)
  if (!root && !llvm::is_contained(own, e.get()))
    return subst(e, m, cache, true);
  if (e->arity() == 0)
    return e;
  llvm::SmallVector<Expr, 4> kids;
  for (size_t i = 0; i < e->arity(); ++i)
    kids.push_back(keep(Expr(e->arg(i)), own, m, cache, false));
  return e->efac().mkNary(e->op(), kids.begin(), kids.end());
}

Expr TopEqFolder::fold(Expr query) {
  for (unsigned round = 0;; ++round) {
    if (round == maxRounds) {
      st.roundCap = true;
      break;
    }
    ExprVector conj;
    conjuncts(query, conj);
    // register the facts; a conjunct owns the keys it defines with the
    // value the map holds (the first definition wins; a conjunct that
    // fixes a key to another value is substituted: false)
    std::vector<std::vector<ENode *>> own(conj.size());
    std::vector<std::tuple<Expr, Expr, Kind>> fs;
    for (size_t i = 0; i < conj.size(); ++i) {
      fs.clear();
      factsOf(conj[i], fs);
      for (auto &[k, v, kind] : fs) {
        auto [it, inserted] = facts.emplace(k.get(), std::make_pair(k, v));
        if (inserted) {
          ++st.facts;
          ++(kind == Kind::Term    ? st.termFacts
             : kind == Kind::Split ? st.splitFacts
                                   : st.boolFacts);
        }
        if (it->second.second == v)
          own[i].push_back(k.get());
      }
    }
    // substitute (one cache over the round), then simplify
    Cache cache;
    ExprVector next;
    next.reserve(conj.size());
    for (size_t i = 0; i < conj.size(); ++i)
      next.push_back(own[i].empty()
                         ? subst(conj[i], facts, cache, true)
                         : keep(conj[i], own[i], facts, cache, true));
    Expr q = simplify(boolop::land(next, query->efac()));
    Expr h = hoistConcatItes(q, concatIte, st);
    if (h != q)
      q = simplify(h);
    if (q == query)
      break;
    ++st.rounds;
    roundMaps.push_back(facts);
    query = q;
  }
  imageCaches.assign(roundMaps.size(), Cache());
  return query;
}

Expr TopEqFolder::image(const Expr &n) {
  Expr r = n;
  for (size_t i = 0; i < roundMaps.size(); ++i) {
    uint64_t uses = st.uses;
    r = simplify(subst(r, roundMaps[i], imageCaches[i], false));
    st.uses = uses;
  }
  return r;
}

} // namespace sea
