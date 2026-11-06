// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/ConstArrayDefs.hh"

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/ExprOpBinder.hh"
#include "sea/Expr/ExprVisitor.hh"

#include "llvm/Support/raw_ostream.h"

#include <iterator>
#include <map>
#include <string>

using namespace expr;

namespace sea {
namespace solver {

bool hasConstArrayLeaf(Expr a) {
  while (true) {
    if (isOpX<CONST_ARRAY>(a))
      return true;
    if (isOpX<STORE>(a)) {
      a = a->arg(0);
      continue;
    }
    if (isOpX<ITE>(a))
      return hasConstArrayLeaf(a->arg(1)) || hasConstArrayLeaf(a->arg(2));
    return false;
  }
}

namespace {
/// \p e with every defined constant substituted, transitively (the map need
/// not be substituted in itself yet; it is acyclic: see add)
Expr expandDefs(Expr e, const ExprMap &defs) {
  while (true) {
    Expr r = expr::replace(e, defs);
    if (r == e)
      return e;
    e = r;
  }
}

/// the name of an array constant, for the name of its free tail
std::string constName(Expr a) {
  Expr n = bind::fname(bind::fname(a));
  if (isOpX<STRING>(n))
    return getTerm<std::string>(n);
  std::string s;
  llvm::raw_string_ostream os(s);
  os << *a;
  return os.str();
}
} // namespace

Expr ConstArrayDefs::apply(Expr e) const {
  return defs.empty() ? e : expr::replace(e, defs);
}

bool ConstArrayDefs::joinsExclusive(
    const std::function<bool(Expr)> &isTrue) const {
  for (const ExprVector &guards : joins) {
    unsigned holding = 0;
    for (Expr g : guards)
      if (isTrue(apply(g)) && ++holding > 1)
        return false;
  }
  return true;
}

Expr ConstArrayDefs::add(Expr exp) {
  // -- an array constant defined by `a = t` where t's store/ite skeleton has
  // -- a const-array leaf: keep the definition and inline it, so selects
  // -- see the const-array. Definitions may come in any order: resolve to a
  // -- fixpoint.
  ExprVector conj;
  {
    ExprVector todo{exp};
    while (!todo.empty()) {
      Expr e = todo.back();
      todo.pop_back();
      if (isOpX<AND>(e))
        todo.insert(todo.end(), e->args_begin(), e->args_end());
      else
        conj.push_back(e);
    }
  }
  std::vector<bool> dropped(conj.size(), false);
  // -- a constant can be defined if its equation is the only thing said about
  // -- it so far (not in an earlier formula: it would stay free there), and
  // -- the definition does not mention it (the equation would be lost)
  auto definable = [&](Expr a) {
    return bind::isArrayConst(a) && !defs.count(a) && !used.count(a);
  };
  // -- joins: a memory named at a join is defined per incoming edge,
  // -- `g_i -> (a = t_i)`; if some t_i reaches a const-array, define a as
  // -- ite(g_1, t_1, ite(g_2, t_2, ... ite(g_n, t_n, a'))), a' a fresh
  // -- constant: a is free if no guard holds (the join did not run), as in
  // -- the equations. Exactly one g_i holds when the join runs: if two do,
  // -- the later ones are ignored (see joinsExclusive)
  for (bool changed = true; changed;) {
    changed = false;
    std::map<Expr, std::vector<size_t>> edges; // array const -> conjuncts
    for (size_t i = 0; i < conj.size(); ++i) {
      Expr c = conj[i];
      if (dropped[i] || !isOpX<IMPL>(c) || !isOpX<EQ>(c->right()))
        continue;
      Expr lhs = c->right()->left();
      if (definable(lhs))
        edges[lhs].push_back(i);
    }
    for (auto &[a, idx] : edges) {
      bool reaches = false;
      for (size_t i : idx)
        reaches |=
            hasConstArrayLeaf(expr::replace(conj[i]->right()->right(), defs));
      if (!reaches)
        continue;
      Expr tail =
          bind::mkConst(mkTerm<std::string>(constName(a) + "!free", a->efac()),
                        bind::rangeTy(a->left()));
      Expr def = tail;
      for (size_t k = idx.size(); k-- > 0;)
        def =
            mk<ITE>(conj[idx[k]]->left(), conj[idx[k]]->right()->right(), def);
      def = expandDefs(def, defs);
      if (contains(def, a))
        continue;
      defs[a] = def;
      if (idx.size() > 1) {
        ExprVector guards;
        for (size_t i : idx)
          guards.push_back(conj[i]->left());
        joins.push_back(guards);
      }
      for (size_t i : idx)
        dropped[i] = true;
      changed = true;
    }
    // -- plain definitions `a = t`
    for (size_t i = 0; i < conj.size(); ++i) {
      Expr c = conj[i];
      if (dropped[i] || !isOpX<EQ>(c))
        continue;
      for (int side = 0; side < 2; ++side) {
        Expr lhs = side ? c->right() : c->left();
        Expr rhs = side ? c->left() : c->right();
        if (!definable(lhs))
          continue;
        Expr r = expandDefs(rhs, defs);
        if (!hasConstArrayLeaf(r) || contains(r, lhs))
          continue;
        defs[lhs] = r;
        dropped[i] = changed = true;
        break;
      }
    }
  }
  // -- definitions that use later definitions (acyclic: every definition
  // -- was expanded with the ones before it, and does not use its constant)
  for (bool changed = true; changed;) {
    changed = false;
    for (auto &kv : defs) {
      Expr r = expr::replace(kv.second, defs);
      changed |= r != kv.second;
      kv.second = r;
    }
  }
  // the kept conjuncts with the definitions substituted: one walk over
  // their conjunction (one memo for the shared subterms), none if nothing
  // is defined (a query with many conjuncts, e.g. Ackermann constraints,
  // was walked once per conjunct)
  ExprVector kept;
  for (size_t i = 0; i < conj.size(); ++i)
    if (!dropped[i])
      kept.push_back(conj[i]);
  exp = kept.empty()
            ? mk<TRUE>(exp->efac())
            : (kept.size() == 1 ? kept[0]
                                : mknary<AND>(kept.begin(), kept.end()));
  if (!defs.empty())
    exp = expr::replace(exp, defs);
  expr::filter(
      exp, [](Expr e) { return bind::isArrayConst(e); },
      std::inserter(used, used.end()));
  return exp;
}

} // namespace solver
} // namespace sea
