// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ParseInput.h - parse a program, keeping positions of names -*- C++
//-*-===//
//
// The MLIR parser turns `loc("name")` into a NameLoc over an unknown
// location: the op's position in the .mlir file is lost. Reports of a run
// (assert-at, assert-op, print at=) show a name together with a position,
// so the tools that report runs parse through parseInputFile, which gives
// every name-only location of an op or block argument its .mlir position
// as the child: loc("name") on line 7, column 3 becomes
// loc("name"("<file>":7:3)) (also at the bottom of a chain of names,
// loc("a"("b"))). A fused location without a file position (a producer's
// link as metadata, loc(fused<"llvm @f %bb #3">[])) gets the position as
// an element: loc(fused<"llvm @f %bb #3">["<file>":7:3]). Names (VC
// symbols, weak print references) do not change.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_INTERP_PARSEINPUT_H
#define SEA_INTERP_PARSEINPUT_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/StringRef.h"

namespace sea {

/// Parses `filename` ("-": stdin) as parseSourceFile<ModuleOp> does, then
/// gives each name-only or position-less fused location its .mlir
/// position (see above). Null on
/// error (reported through the context's diagnostics).
mlir::OwningOpRef<mlir::ModuleOp> parseInputFile(llvm::StringRef filename,
                                                 mlir::MLIRContext *context);

} // namespace sea

#endif // SEA_INTERP_PARSEINPUT_H
