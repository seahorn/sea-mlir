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
    read before any write gives the variable's initial value: integers
    start nondet, named by `name` and `site` (`sea.nondet "<name>" [<site>]`);
    memories (regions) start with nondet contents
    (`sea.mem.nondet "<name>" [<site>]`); pointers start null
    (`sea.ptr.null`); structs are `sea.struct.nondet` (one bit-vector of
    the integer and address leaves, pointer leaves null).
    Programs are expected to set a variable before they get it; a pointer
    read before it is set is not given a meaning beyond null. Every read
    before a write sees the same value. `site` is unique with
    the program's other nondet sites.

    The op is in the entry block of a `sea.program`. Its result is used
    only as the variable operand of seaimp ops (`get`, `set`, the region
    accesses, `alloca`) in the same region (not as a block argument, a
    select operand or a stored value): every verified variable is promoted
    by the lowering.

    ```mlir
    %x = seaimp.var "x" [7] : !seaimp.var<i32>
    %mem = seaimp.var "mem" [0] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
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

//===----------------------------------------------------------------------===//
// Region accesses: the sea memory ops on the memory a variable holds. Each
// means `get` the memory, apply the sea op `inplace`, `set` the result (the
// update ops) or `get` and apply (load). The operands and attributes are
// those of the sea op, minus the memory operand, the memory result and
// `inplace`; `tracked` is not written in seaimp (sea-apply-tracking sets it
// after the lowering).
//===----------------------------------------------------------------------===//

def SeaImp_MemVar : Type<
    CPred<"::llvm::isa<::sea::imp::VarType>($_self) && ::llvm::isa<::sea::MemType>("
          "::llvm::cast<::sea::imp::VarType>($_self).getValueType())">,
    "seaimp variable holding a memory", "::sea::imp::VarType">;
def SeaImp_Index : Type<CPred<"::llvm::isa<::sea::AddrType, ::sea::PtrType>($_self)">,
                        "address or pointer">;
def SeaImp_BV : Type<CPred<"::llvm::isa<::mlir::IntegerType>($_self)">,
                     "integer">;
def SeaImp_CellValue : Type<
    CPred<"::llvm::isa<::mlir::IntegerType, ::sea::AddrType, ::sea::PtrType>($_self)">,
    "integer, address or pointer">;

// `$_self` is the variable's type; the region's index / length / unit
// type, or NoneType for a variable that holds no memory (the parser then
// reports a type mismatch, and the verifier the variable's type).
class SeaImp_IndexOf<string var, string addr> : TypesMatchWith<
    "index type indexes the region", var, addr,
    "::sea::imp::regionIndexType($_self)">;
class SeaImp_LenOf<string var, string len> : TypesMatchWith<
    "length has the index width", var, len,
    "::sea::imp::regionLenType($_self)">;

def SeaImp_LoadOp : SeaImp_Op<"load", [SeaImp_IndexOf<"var", "addr">]> {
  let summary = "Read the cells at an address of a region";
  let description = [{
    `sea.load` on the memory the variable holds: `get`, then load.

    ```mlir
    %x = seaimp.load %mem[%p] align 4 : !seaimp.var<!m> -> i32
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead]>:$var,
                   SeaImp_Index:$addr,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align);
  let results = (outs SeaImp_CellValue:$result);
  let assemblyFormat = [{
    $var `[` $addr `]` (`align` $align^)? attr-dict `:` qualified(type($var))
    `->` qualified(type($result))
  }];
  let hasVerifier = 1;
}

def SeaImp_StoreOp : SeaImp_Op<"store", [SeaImp_IndexOf<"var", "addr">]> {
  let summary = "Write a value to a region";
  let description = [{
    `sea.store` on the memory the variable holds: `get`, store `inplace`,
    `set`.

    ```mlir
    seaimp.store %mem[%p], %v align 4 : !seaimp.var<!m>, i32
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead, MemWrite]>:$var,
                   SeaImp_Index:$addr, SeaImp_CellValue:$value,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $value (`align` $align^)? attr-dict `:`
    qualified(type($var)) `,` qualified(type($value))
  }];
  let hasVerifier = 1;
}

def SeaImp_MemSetOp : SeaImp_Op<"memset", [
    SeaImp_IndexOf<"var", "addr">, SeaImp_LenOf<"var", "len">,
    TypesMatchWith<"value is one index unit", "var", "value",
                   "::sea::imp::regionUnitType($_self)">]> {
  let summary = "Set `len` index units of a region to a unit value";
  let description = [{
    `sea.memset` on the memory the variable holds: `get`, memset
    `inplace`, `set`.

    ```mlir
    seaimp.memset %mem[%buf], %z8, %c16 : !seaimp.var<!m>
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead, MemWrite]>:$var,
                   SeaImp_Index:$addr, SeaImp_BV:$value, SeaImp_BV:$len,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $value `,` $len (`align` $align^)? attr-dict `:`
    qualified(type($var))
  }];
  let hasVerifier = 1;
}

def SeaImp_MemMoveOp : SeaImp_Op<"memmove", [
    AllTypesMatch<["var", "src"]>, SeaImp_IndexOf<"var", "addr">,
    SeaImp_IndexOf<"var", "srcAddr">, SeaImp_LenOf<"var", "len">]> {
  let summary = "Copy `len` cells from a region to a region";
  let description = [{
    `sea.memmove` on the memories the variables hold: `get` both (one
    `get` if they are the same variable), memmove `inplace`, `set` the
    destination. The source is read as it was before the op, so overlap is
    harmless. With one region, `src` and `var` are the same variable.

    ```mlir
    seaimp.memmove %mem[%dst], %mem[%buf], %c8 : !seaimp.var<!m>
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead, MemWrite]>:$var,
                   SeaImp_Index:$addr,
                   Arg<SeaImp_MemVar, "", [MemRead]>:$src,
                   SeaImp_Index:$srcAddr, SeaImp_BV:$len,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$src_align);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $src `[` $srcAddr `]` `,` $len
    (`align` $align^)? (`src_align` $src_align^)? attr-dict `:`
    qualified(type($var))
  }];
  let hasVerifier = 1;
}

def SeaImp_MemHavocOp : SeaImp_Op<"memhavoc", [
    Sea_NondetSiteOpInterface,
    SeaImp_IndexOf<"var", "addr">, SeaImp_LenOf<"var", "len">]> {
  let summary = "Make `len` cells of a region nondeterministic";
  let description = [{
    `sea.memhavoc` on the memory the variable holds: `get`, memhavoc
    `inplace`, `set`. `site` is unique with the program's other nondet
    sites; the lowered memhavoc keeps it.

    ```mlir
    seaimp.memhavoc %mem[%buf], %c16 [5] : !seaimp.var<!m>
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead, MemWrite]>:$var,
                   SeaImp_Index:$addr, SeaImp_BV:$len,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $len ` ` `[` $site `]` attr-dict `:`
    qualified(type($var))
  }];
  let hasVerifier = 1;
}

#endif // SEAIMP_OPS
