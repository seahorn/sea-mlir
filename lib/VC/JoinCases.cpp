// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- JoinCases.cpp - joins built as cases terms -------------*- C++ -*-===//

#include "sea/VC/JoinCases.h"

#include "llvm/ADT/DenseMap.h"

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
  return op::cases::expand(c);
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
}

} // namespace sea
