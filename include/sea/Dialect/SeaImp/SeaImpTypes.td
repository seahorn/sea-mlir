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
    seaimp ops may use it. `T` is a signless integer, a pointer
    (`!sea.ptr`), a struct (`!sea.struct`) or a memory (`!sea.mem`: a
    region, accessed by the region ops). Addresses are not supported.

    ```mlir
    !seaimp.var<i32>
    !seaimp.var<!sea.ptr<64>>
    !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
    ```
  }];
  let parameters = (ins "::mlir::Type":$valueType);
  let assemblyFormat = "`<` qualified($valueType) `>`";
  let genVerifyDecl = 1;
}

def SeaImp_Var : Type<CPred<"::llvm::isa<::sea::imp::VarType>($_self)">,
                      "seaimp variable handle", "::sea::imp::VarType">;

#endif // SEAIMP_TYPES
