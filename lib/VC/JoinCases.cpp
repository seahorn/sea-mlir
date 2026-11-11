// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- JoinCases.cpp - joins built as cases terms -------------*- C++ -*-===//

#include "sea/VC/JoinCases.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

// Expr.hh declares boost::hash_combine only; DefOp::hash needs it defined
#include <boost/functional/hash.hpp>

#include <algorithm>

using namespace expr;

namespace sea {

Expr JoinCases::join(llvm::ArrayRef<Expr> edges, llvm::ArrayRef<Expr> vals,
                     JoinKind kind, std::string &why) {
  assert(!vals.empty() && edges.size() == vals.size());
  if (vals.size() == 1)
    return vals.front();
  ExprVector args;
  args.reserve(2 * vals.size() - 1);
  for (size_t j = 0; j + 1 < vals.size(); ++j) {
    args.push_back(edges[j]);
    args.push_back(vals[j]);
  }
  args.push_back(vals.back());
  Expr c = op::cases::make(args, tc, why);
  if (!c)
    return Expr();
  count(edges, vals, kind);
  c = reorder(c, reorderOpts, kind);
  return isOpX<CASES>(c) ? op::cases::expand(c) : c;
}

Expr JoinCases::reorder(Expr c, Reorder r, JoinKind kind) {
  assert(isOpX<CASES>(c));
  if (!r.merge && !r.dropDefault)
    return c;
  const unsigned k = static_cast<unsigned>(kind);
  ExprFactory &efac = c->efac();
  const Expr dflt = c->last();
  // the run of pairs equal to the default at the end: dropped, as the ite
  // chain drops it (no negation needed, nothing follows)
  size_t end = c->arity() / 2;
  while (end > 0 && c->arg(2 * end - 1) == dflt)
    --end;

  struct Pair {
    Expr cond, val;
  };
  llvm::SmallVector<Pair> out;
  // the negations of the conditions of the dropped pairs so far
  ExprVector dropped;
  bool changed = false;
  for (size_t j = 0; j < end; ++j) {
    Expr cj = c->arg(2 * j), vj = c->arg(2 * j + 1);
    if (r.dropDefault && vj == dflt) {
      // first-match: the pairs after j hold only when c_j does not
      dropped.push_back(boolop::lneg(cj));
      ++reorderDropped[k];
      changed = true;
      continue;
    }
    // c_j in the term with the earlier dropped pairs removed
    ExprVector conj{cj};
    conj.insert(conj.end(), dropped.begin(), dropped.end());
    reorderDropNegations[k] += dropped.size();
    auto *it =
        r.merge ? llvm::find_if(out, [&](const Pair &p) { return p.val == vj; })
                : out.end();
    if (it == out.end()) {
      out.push_back({boolop::land(conj, efac), vj});
      continue;
    }
    // pair j moves ahead of the pairs after i: c_i | (c_j & !c_(i+1) & ...)
    const size_t i = it - out.begin();
    for (size_t q = i + 1; q < out.size(); ++q)
      conj.push_back(boolop::lneg(out[q].cond));
    it->cond = boolop::lor(it->cond, boolop::land(conj, efac));
    ++reorderMerged[k];
    if (i + 1 == out.size())
      ++reorderMergedAdjacent[k];
    reorderNegations[k] += out.size() - i - 1;
    changed = true;
  }
  reorderPairs[k] += out.size();
  if (changed)
    ++reorderJoins[k];
  else if (end == c->arity() / 2)
    return c;
  if (out.empty())
    return dflt;
  ExprVector args;
  args.reserve(2 * out.size() + 1);
  for (const Pair &p : out) {
    args.push_back(p.cond);
    args.push_back(p.val);
  }
  args.push_back(dflt);
  std::string why;
  Expr res = op::cases::make(args, tc, why);
  assert(res && "a rearranged cases term is well-sorted");
  return res;
}

void JoinCases::count(llvm::ArrayRef<Expr> edges, llvm::ArrayRef<Expr> vals,
                      JoinKind kind) {
  const unsigned k = static_cast<unsigned>(kind);
  const size_t n = vals.size();
  ++built[k];
  ++arity[n][k];
  values[k] += n;

  llvm::DenseMap<ENode *, unsigned> mult;
  for (const Expr &v : vals)
    ++mult[v.get()];
  const size_t d = mult.size();
  distinct[k] += d;
  if (d < n)
    ++withDup[k];
  redundant[k] += n - d;
  if (d == 1)
    ++allEqual[k];

  size_t adj = 0;
  for (size_t i = 0; i + 1 < n; ++i)
    if (vals[i] == vals[i + 1])
      ++adj;
  adjacentEqual[k] += adj;
  if (vals[n - 2] == vals[n - 1])
    ++lastEqualDefault[k];
  // the pairs the ite chain drops already (lite(c, v, v) = v): the run of
  // values equal to the default at the end
  for (size_t i = n - 1; i-- > 0 && vals[i] == vals[n - 1];)
    ++iteFolded[k];
  // after merging every run of adjacent equal values n - adj entries are
  // left; those above the number of distinct values need a reordering
  const size_t rest = n - adj - d;
  nonAdjacent[k] += rest;
  if (rest)
    ++withNonAdjacent[k];

  unsigned most = 0;
  for (auto &[node, m] : mult)
    most = std::max(most, m);
  if (mult.lookup(vals.back().get()) < most)
    ++defaultNotMostFrequent[k];
}

void JoinCases::print(llvm::raw_ostream &os) const {
  auto line = [&](llvm::StringRef name, const Count &c) {
    os << "cases." << name << ": " << (c[0] + c[1] + c[2]) << " scalar=" << c[0]
       << " memory=" << c[1] << " companion=" << c[2] << "\n";
  };
  // the joins built as cases (one per block argument or companion at a
  // block with two or more incoming edges)
  line("joins", built);
  for (const auto &[n, c] : arity)
    line("arity." + std::to_string(n), c);
  line("values", values);
  line("distinct-values", distinct);
  line("with-dup", withDup);
  line("redundant-values", redundant);
  line("all-equal", allEqual);
  line("adjacent-equal-pairs", adjacentEqual);
  line("ite-folded-pairs", iteFolded);
  line("last-equal-default", lastEqualDefault);
  line("nonadjacent-redundant", nonAdjacent);
  line("with-nonadjacent", withNonAdjacent);
  line("default-not-most-frequent", defaultNotMostFrequent);
  if (!reorderOpts.merge && !reorderOpts.dropDefault)
    return;
  // --vc-cases-reorder / --vc-cases-drop-default
  line("reorder-joins", reorderJoins);
  line("reorder-merged-pairs", reorderMerged);
  line("reorder-merged-adjacent", reorderMergedAdjacent);
  line("reorder-merge-negations", reorderNegations);
  line("reorder-dropped-default", reorderDropped);
  line("reorder-drop-negations", reorderDropNegations);
  line("reorder-pairs", reorderPairs);
}

} // namespace sea
