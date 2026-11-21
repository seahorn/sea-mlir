// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpTypes.td - seaimp dialect types --------------*- tablegen -*-===//

#ifndef SEAIMP_TYPES
#define SEAIMP_TYPES

include "sea/Dialect/SeaImp/SeaImpDialect.td"
include "mlir/IR/AttrTypeBase.td"

def SeaImp_VarType : TypeDef<SeaImp_Dialect, "Var"> {
  let mnemonic = "var";
  let summary = "A handle to a variable holding a value of type `T`";
  let description = [{
    The handle of a `seaimp.var`. It is not a program value: only
    `seaimp.get` and `seaimp.set` may use it. `T` is a sea register type:
    a signless integer, a pointer (`!sea.ptr`) or a struct
    (`!sea.struct`). Addresses and memories are not supported yet.

    ```mlir
    !seaimp.var<i32>
    !seaimp.var<!sea.ptr<64>>
    ```
  }];
  let parameters = (ins "::mlir::Type":$valueType);
  let assemblyFormat = "`<` qualified($valueType) `>`";
  let genVerifyDecl = 1;
}

def SeaImp_Var : Type<CPred<"::llvm::isa<::sea::imp::VarType>($_self)">,
                      "seaimp variable handle", "::sea::imp::VarType">;

#endif // SEAIMP_TYPES
