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
// Two opt-in rearrangements (sea-vc --vc-cases-reorder,
// --vc-cases-drop-default; each changes the query) rewrite the cases term
// before the expansion, by first-match semantics alone (no exclusivity of the
// edges is used; a later simplifier may use it to drop the negations they add):
//  - merge: a pair j whose value is the value of an earlier pair i (the same
//    node) merges into pair i with the condition
//    c_i | (c_j & !c_(i+1) & ... & !c_(j-1)): moving pair j ahead conjoins the
//    negations of the conditions it skips;
//  - drop-default: a pair whose value is the default is dropped, and the
//    negation of its condition is conjoined into the conditions of the pairs
//    after it.
// The run of pairs equal to the default at the end is dropped first (exact:
// the ite chain drops it too). Each is counted (`cases.reorder-*` lines,
// printed only when a rearrangement is on).
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
  /// The rearrangements of the cases term before its expansion (off: the
  /// query is the one without --vc-join-cases).
  struct Reorder {
    bool merge = false;       // --vc-cases-reorder
    bool dropDefault = false; // --vc-cases-drop-default
  };

  JoinCases() = default;
  explicit JoinCases(Reorder r) : reorderOpts(r) {}

  /// The join of `vals` over `edges` (one value per edge, the last value the
  /// default; the last edge is not used): built as a cases term (checked by
  /// its constructor), counted, and returned expanded. With one edge returns
  /// vals[0]. On a refused construction returns null and sets `why`.
  expr::Expr join(llvm::ArrayRef<expr::Expr> edges,
                  llvm::ArrayRef<expr::Expr> vals, JoinKind kind,
                  std::string &why);

  /// The cases term \p c rearranged by \p r (see the file comment), counted
  /// as a join of \p kind: a cases term, or the default value when no pair is
  /// left. The term has no run of pairs equal to the default at its end, and
  /// with merge no two pairs with one value, with drop-default no pair equal
  /// to the default. With neither rearrangement on returns \p c.
  expr::Expr reorder(expr::Expr c, Reorder r, JoinKind kind);

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
  // the rearrangements: joins changed, pairs merged (of which with no pair
  // between them), negations conjoined by merges, pairs dropped as equal to
  // the default, negations conjoined by drops, pairs left
  Count reorderJoins{}, reorderMerged{}, reorderMergedAdjacent{},
      reorderNegations{}, reorderDropped{}, reorderDropNegations{},
      reorderPairs{};
  Reorder reorderOpts;
  std::map<size_t, Count> arity;
};

} // namespace sea

#endif // SEA_VC_JOINCASES_H
