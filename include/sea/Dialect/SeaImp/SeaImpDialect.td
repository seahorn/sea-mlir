// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpDialect.td - seaimp dialect definition -------*- tablegen -*-===//

#ifndef SEAIMP_DIALECT
#define SEAIMP_DIALECT

include "mlir/IR/OpBase.td"

def SeaImp_Dialect : Dialect {
  let name = "seaimp";
  let summary = "Imperative layer over sea: variables, lowered to sea.";
  let description = [{
    `seaimp` holds what is convenient to write but not SSA: variables
    (`seaimp.var`, read and written whole by `seaimp.get` / `seaimp.set`).
    It has no runtime semantics of its own: a program means what its
    lowering to sea means (`sea-lower-seaimp`, MLIR's Mem2Reg). The
    interpreter and the VC generator refuse seaimp ops
    (docs/design/SeaImp.md).
  }];
  let cppNamespace = "::sea::imp";
  let useDefaultTypePrinterParser = 1;
  // the lowering makes sea.nondet / sea.struct.nondet / sea.ptr.nondet
  let dependentDialects = ["::sea::SeaDialect"];
}

class SeaImp_Op<string mnemonic, list<Trait> traits = []> :
    Op<SeaImp_Dialect, mnemonic, traits>;

#endif // SEAIMP_DIALECT
