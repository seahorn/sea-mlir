// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Origins.h - VC terms -> the sea ops that made them -------*- C++ -*-===//
//
// A side table from Expr nodes to the locations of the sea ops whose encoding
// made them (sea-vc --arith-stats, --ack-dump-pairs). SeaOpSem fills it as it
// encodes an op (SeaOpSem::setOrigins): the arithmetic nodes of mul / div /
// rem / mul-overflow / ptr.index, and the selects of a load. A node made by
// several ops (hash-consing) has each op's location.
//
// The terms go through stages before the solver gets them (simplify, the
// solver boundary, the Ackermann lowering), and each stage may rebuild a node
// whose kids changed. The table follows a stage two ways:
//  - through(img): a stage with a map from its input nodes to its output
//    nodes (the rewriter's cache, the Ackermann memo);
//  - along(pre, post): a stage that keeps the term structure where it does
//    not translate (the boundary: store-map expansion, pointer erasure): the
//    two roots are walked in step, and a node of post at the same position
//    and with the same operator as a node of pre gets pre's locations; the
//    walk stops where the operators or arities differ.
// A node rewritten into a different operator, or made by a stage (SeaM
// rules, the boundary's expansions), has no origin: the reports count it as
// unattributed. The table keeps its nodes alive.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_ORIGINS_H
#define SEA_VC_ORIGINS_H

#include "sea/Expr/Expr.hh"

#include "mlir/IR/Location.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <vector>

namespace sea {

class ExprOrigins {
public:
  /// \p n was made by the op at \p loc (a location once per node)
  void add(const expr::Expr &n, mlir::Location loc);
  /// the locations of \p n (empty: unattributed)
  llvm::ArrayRef<mlir::Location> of(const expr::ENode *n) const;
  size_t size() const { return map.size(); }
  bool empty() const { return map.empty(); }

  /// the table of a stage's output: every node's locations go to \p img(n)
  /// (null: the node has no image, its locations are dropped)
  ExprOrigins
  through(llvm::function_ref<expr::Expr(const expr::Expr &)> img) const;
  /// the table of a structure-keeping stage that turned \p pre into \p post
  /// (see the file comment)
  ExprOrigins along(const expr::Expr &pre, const expr::Expr &post) const;

private:
  llvm::DenseMap<const expr::ENode *, llvm::SmallVector<mlir::Location, 1>> map;
  std::vector<expr::Expr> keep;
};

} // namespace sea

#endif // SEA_VC_ORIGINS_H
