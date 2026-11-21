// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpOps.td - seaimp dialect operations -----------*- tablegen -*-===//

#ifndef SEAIMP_OPS
#define SEAIMP_OPS

include "sea/Dialect/SeaImp/SeaImpDialect.td"
include "sea/Dialect/SeaImp/SeaImpTypes.td"
include "sea/Dialect/Sea/SeaInterfaces.td"
include "mlir/Interfaces/MemorySlotInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def SeaImp_VarOp : SeaImp_Op<"var", [
    Sea_NondetSiteOpInterface,
    DeclareOpInterfaceMethods<PromotableAllocationOpInterface>]> {
  let summary = "Declare a variable";
  let description = [{
    A variable holding one value of type `T`, read and written whole. A
    read before any write gives the variable's initial value, one
    nondeterministic value named by `name` and `site`: the lowering makes
    it `sea.nondet "<name>" [<site>]` (integers), `sea.struct.nondet`
    (structs: one bit-vector of the integer and address leaves, pointer
    leaves null) or `sea.ptr.nondet` (pointers: never dereferenceable).
    Every read before a write sees the same value. `site` is unique with
    the program's other nondet sites.

    The op is in the entry block of a `sea.program`. Its result is used
    only as the variable operand of `seaimp.get` / `seaimp.set` in the
    same region (not as a block argument, a select operand or a stored
    value): every verified variable is promoted by the lowering.

    ```mlir
    %x = seaimp.var "x" [7] : !seaimp.var<i32>
    ```
  }];
  let arguments = (ins StrAttr:$name,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site);
  let results = (outs Res<SeaImp_Var, "", [MemAlloc]>:$result);
  let assemblyFormat =
      "$name ` ` `[` $site `]` attr-dict `:` qualified(type($result))";
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    ::mlir::Type getValueType() {
      return ::llvm::cast<VarType>(getResult().getType()).getValueType();
    }
  }];
}

def SeaImp_GetOp : SeaImp_Op<"get", [
    DeclareOpInterfaceMethods<PromotableMemOpInterface>,
    TypesMatchWith<"result type is the variable's value type", "var",
                   "result",
                   "::llvm::cast<::sea::imp::VarType>($_self).getValueType()">
  ]> {
  let summary = "Read a variable";
  let description = [{
    The value the variable holds: the last `seaimp.set` before it on the
    path, or the variable's initial value.

    ```mlir
    %v = seaimp.get %x : !seaimp.var<i32>
    ```
  }];
  let arguments = (ins Arg<SeaImp_Var, "", [MemRead]>:$var);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$var attr-dict `:` qualified(type($var))";
  let hasVerifier = 1;
}

def SeaImp_SetOp : SeaImp_Op<"set", [
    DeclareOpInterfaceMethods<PromotableMemOpInterface>,
    TypesMatchWith<"value type is the variable's value type", "var",
                   "value",
                   "::llvm::cast<::sea::imp::VarType>($_self).getValueType()">
  ]> {
  let summary = "Write a variable";
  let description = [{
    Replaces the value the variable holds.

    ```mlir
    seaimp.set %x, %v : !seaimp.var<i32>
    ```
  }];
  let arguments = (ins Arg<SeaImp_Var, "", [MemWrite]>:$var, AnyType:$value);
  let assemblyFormat = "$var `,` $value attr-dict `:` qualified(type($var))";
  let hasVerifier = 1;
}

#endif // SEAIMP_OPS
