// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Names.h - solver names for values and blocks -------------*- C++ -*-===//
//
// The names of VC symbols. A value or block is named exactly as `sea-opt
// --mlir-use-nameloc-as-prefix` prints it (`%x`, `%x_0`, `%12`, `^bb3`), so
// dumps, traces and the printed program agree. NameLoc locations are the name
// hints; MLIR's AsmState makes them unique per program. Nondet results are
// named by their site (`nd.<site>`), the stable key for replay.
//
// The sigils keep the three kinds of names apart: `%` values, `^` blocks,
// `nd.` nondets.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_NAMES_H
#define SEA_VC_NAMES_H

#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/IR/AsmState.h"
#include "llvm/ADT/DenseMap.h"

namespace sea {

class ValueNames {
public:
  explicit ValueNames(ProgramOp program);

  mlir::StringAttr name(mlir::Value v);
  mlir::StringAttr name(mlir::Block *b);

private:
  mlir::MLIRContext *ctx;
  mlir::AsmState state;
  llvm::DenseMap<mlir::Value, mlir::StringAttr> values;
  llvm::DenseMap<mlir::Block *, mlir::StringAttr> blocks;
};

} // namespace sea

#endif // SEA_VC_NAMES_H
