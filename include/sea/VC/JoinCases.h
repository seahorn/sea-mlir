// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- JoinCases.h - joins built as cases terms (experimental) --*- C++ -*-===//
//
// sea-vc --vc-join-cases (off by default, a prototype): VCGen builds the ite
// join of a block argument (a value or a memory) and of a memory's pointer
// companion (SeaOpSem::joinCompanions) as one term
// cases(e0, v0, ..., e(n-2), v(n-2), v(n-1)) over the n incoming edges, the
// last edge's value the default, as the ite join does. The term is counted
// and expanded at once (expr::op::cases::expand) to the ite chain the join
// builds without the option, before the value is written to the store: no
// rewriter, OpSem rule, solver or evaluator sees `cases`, and the query is
// the same as without the option. A join over one edge is its value, as
// without the option: no cases term, not counted.
//
// The counters (`cases.*` lines, print) measure where an n-ary join term
// would matter: repeated values among the n values, the ones a sequential
// merge of adjacent equal values folds (first-match semantics allows it
// without exclusivity) and the ones only a reordering could fold (rotation of
// the default, or an operator that uses the exclusivity of the edges).
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_JOINCASES_H
#define SEA_VC_JOINCASES_H

#include "sea/Expr/Expr.hh"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <map>
#include <string>

namespace sea {

/// What a join defines: a non-memory block argument (`scalar`), a memory
/// block argument, or a pointer companion.
enum class JoinKind { Value, Memory, Companion };

class JoinCases {
public:
  /// The join of `vals` over `edges` (one value per edge, the last value the
  /// default; the last edge is not used): built as a cases term (checked by
  /// its constructor), counted, and returned expanded. With one edge returns
  /// vals[0]. On a refused construction returns null and sets `why`.
  expr::Expr join(llvm::ArrayRef<expr::Expr> edges,
                  llvm::ArrayRef<expr::Expr> vals, JoinKind kind,
                  std::string &why);

  /// The counters, one `cases.<name>: <total> scalar=<n> memory=<n>
  /// companion=<n>` line each.
  void print(llvm::raw_ostream &os) const;

private:
  using Count = std::array<uint64_t, 3>;
  void count(llvm::ArrayRef<expr::Expr> edges, llvm::ArrayRef<expr::Expr> vals,
             JoinKind kind);

  expr::TypeChecker tc;
  Count built{}, values{}, distinct{}, withDup{}, redundant{}, allEqual{},
      adjacentEqual{}, iteFolded{}, lastEqualDefault{}, nonAdjacent{},
      withNonAdjacent{}, defaultNotMostFrequent{};
  std::map<size_t, Count> arity;
};

} // namespace sea

#endif // SEA_VC_JOINCASES_H
