// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ArithShape.hh - arithmetic nodes, operand shape ----------*- C++ -*-===//
//
// Shared by the reports on the arithmetic of a VC (sea-vc --arith-stats,
// --split-candidates' arithmetic ranking, --ite-stats) and by the origin
// table that SeaOpSem fills (sea/VC/Origins.h):
//  - arithKind / isArith: the arithmetic nodes, bvmul, bvudiv, bvsdiv,
//    bvurem, bvsrem, bvsmod and the multiplication overflow predicates;
//  - ValueTrees: the value tree of an operand, and, a separate query on the
//    same memoized walk, whether an ite is anywhere below a term.
//
// The value tree of a term goes through ite branches, concat parts, the
// value of extract (its last argument), sign / zero extension, bvneg and
// bvnot; any other operator is a leaf. A term is path-merged if its value
// tree holds an ite: a value stored on several paths and loaded as bytes is
// a concat of ites, and its byte pieces are extracts of them.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_EXPR_ARITHSHAPE_HH
#define SEA_EXPR_ARITHSHAPE_HH

#include "sea/Expr/Expr.hh"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace expr {

/// Mul: bvmul and the multiplication overflow predicates; Div: bvudiv,
/// bvsdiv, bvurem, bvsrem, bvsmod; None: any other node.
enum class ArithKind { None, Mul, Div };
ArithKind arithKind(const ENode *n);
inline bool isArith(const ENode *n) { return arithKind(n) != ArithKind::None; }

/// The value-tree kids of \p n (see the file comment) into \p ks: none for
/// a leaf.
void valueTreeKids(ENode *n, llvm::SmallVectorImpl<ENode *> &ks);

/// Queries on terms, memoized per node (the nodes must outlive the object).
class ValueTrees {
public:
  struct Info {
    /// the value tree holds an ite (path-merged)
    bool ite = false;
    /// every leaf of the value tree is a numeral
    bool allNum = true;
  };
  /// the value tree of \p root
  Info info(ENode *root);
  /// the ite conditions of the value tree of \p root, into \p conds
  static void conds(ENode *root, llvm::DenseSet<const ENode *> &conds);
  /// An ite anywhere in the DAG of \p root, \p root included, through every
  /// operator (an address `base + 4 * ite(..)`: an index chosen by a path).
  /// Not the value tree: an operand whose ite is below an add is not
  /// path-merged.
  bool iteBelow(ENode *root);

private:
  llvm::DenseMap<const ENode *, Info> infoMemo;
  llvm::DenseMap<const ENode *, bool> iteMemo;
};

} // namespace expr

#endif // SEA_EXPR_ARITHSHAPE_HH
