// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaDialect.td - sea dialect definition -------------*- tablegen -*-===//

#ifndef SEA_DIALECT
#define SEA_DIALECT

include "mlir/IR/OpBase.td"

def Sea_Dialect : Dialect {
  let name = "sea";
  let summary = "Verification IR for SeaHorn-style VC generation.";
  let description = [{
    The `sea` dialect holds the programs SeaHorn generates verification
    conditions from. Values are bit-vectors: builtin signless integers `iN`.
    `i1` is a 1-bit bit-vector; whether it becomes an SMT Bool or a `(_ BitVec
    1)` is decided by the VC encoding, not by the dialect.

    Every operation is total. Arithmetic follows SMT-LIB bit-vector semantics
    (e.g. `bvudiv x 0 = ~0`, shifts by at least the width give 0 or the sign
    fill). There is no poison or undef.
  }];
  let cppNamespace = "::sea";
  let useDefaultTypePrinterParser = 1;
  // sea.const for folded integers (sccp, canonicalize).
  let hasConstantMaterializer = 1;
  let extraClassDeclaration = [{
    void registerTypes();
  }];
}

class Sea_Op<string mnemonic, list<Trait> traits = []> :
    Op<Sea_Dialect, mnemonic, traits>;

#endif // SEA_DIALECT
