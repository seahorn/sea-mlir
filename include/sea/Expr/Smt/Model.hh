// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
/* Generic API for a model */

#include "sea/Expr/ExprAPInt.hh"

namespace sea {
namespace solver {

class Model {
public:
  virtual ~Model() {}

  virtual expr::Expr eval(expr::Expr expr, bool complete) = 0;

  /// Is the cell \p arr[\p idx] (\p arr an array constant, \p idx a
  /// numeral) a default of this model: no constraint of the query reads it,
  /// so every value is consistent with the model and eval's value is not a
  /// fact of it? False when not known (expr::ReadCellsModel answers it for a
  /// solver's model; the models of the query lowerings for their arrays).
  virtual bool defaultCell(expr::Expr arr, expr::Expr idx) { return false; }

  virtual void print(llvm::raw_ostream &o) const = 0;

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &o,
                                       const Model &model) {
    model.print(o);
    return o;
  }
};
} // namespace solver
} // namespace sea
