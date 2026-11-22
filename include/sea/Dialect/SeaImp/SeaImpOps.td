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
    accesses and allocations) in the same region, or of a region read in a
    `sea.box.all` body directly in it (not as a block argument, a select
    operand or a stored value): every verified variable is promoted by the
    lowering. A memory variable (a region) has no `get` / `set`.

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
    path, or the variable's initial value. Not on a memory variable (a
    region): memory is read only through the region ops, so seaimp code
    never holds a memory version.

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
    Replaces the value the variable holds. Not on a memory variable (a
    region): memory is written only through the region ops.

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
// update ops) or `get` and apply (the reads). The operands and attributes
// are those of the sea op, minus the memory operand, the memory result and
// `inplace`. `tracked` on an update is kept as written; programs usually
// mark tracking with sea.tracking_on / tracking_off, which
// sea-apply-tracking turns into `tracked` after the lowering.
//===----------------------------------------------------------------------===//

def SeaImp_MemVar : Type<
    CPred<"::llvm::isa<::sea::imp::VarType>($_self) && ::llvm::isa<::sea::MemType>("
          "::llvm::cast<::sea::imp::VarType>($_self).getValueType())">,
    "seaimp variable holding a memory", "::sea::imp::VarType">;
def SeaImp_Index : Type<CPred<"::llvm::isa<::sea::AddrType, ::sea::PtrType>($_self)">,
                        "address or pointer">;
def SeaImp_Ptr : Type<CPred<"::llvm::isa<::sea::PtrType>($_self)">, "pointer">;
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

def SeaImp_LoadOp : SeaImp_Op<"load", [SeaImp_IndexOf<"var", "addr">,
                                       Sea_BoxBodyReadOpInterface]> {
  let summary = "Read the cells at an address of a region";
  let description = [{
    `sea.load` on the memory the variable holds: `get`, then load. It may
    stand in a `sea.box.all` body directly in the variable's region (the
    body only reads); the memory is then read before the box.

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
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $value (`align` $align^)? (`tracked` $tracked^)?
    attr-dict `:` qualified(type($var)) `,` qualified(type($value))
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
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $value `,` $len (`align` $align^)?
    (`tracked` $tracked^)? attr-dict `:` qualified(type($var))
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
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$src_align,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $src `[` $srcAddr `]` `,` $len
    (`align` $align^)? (`src_align` $src_align^)? (`tracked` $tracked^)?
    attr-dict `:` qualified(type($var))
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
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $var `[` $addr `]` `,` $len ` ` `[` $site `]` (`tracked` $tracked^)?
    attr-dict `:` qualified(type($var))
  }];
  let hasVerifier = 1;
}

def SeaImp_IsModifiedOp : SeaImp_Op<"is_modified", [
    SeaImp_IndexOf<"var", "ptr">, Sea_BoxBodyReadOpInterface]> {
  let summary = "1 if a tracked update wrote to `ptr`'s object in a region";
  let description = [{
    `sea.mem.is_modified` on the memory the variable holds: `get`, then
    is_modified. Part of the specification language (SeaHorn's
    `sea_is_modified`), not a debugging op. May stand in a `sea.box.all`
    body directly in the variable's region, as `seaimp.load`.

    ```mlir
    %f = seaimp.is_modified %mem[%p] : !seaimp.var<!m>
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead]>:$var,
                   SeaImp_Ptr:$ptr);
  let results = (outs I1:$result);
  let assemblyFormat =
      "$var `[` $ptr `]` attr-dict `:` qualified(type($var))";
  let hasVerifier = 1;
}

def SeaImp_ResetModifiedOp : SeaImp_Op<"reset_modified", [
    SeaImp_IndexOf<"var", "addr">]> {
  let summary = "Forget that `ptr`'s object in a region was modified";
  let description = [{
    `sea.mem.reset_modified` on the memory the variable holds: `get`,
    reset_modified `inplace`, `set`. The contents and the modified state of
    the other objects stay. Part of the specification language (SeaHorn's
    `sea_reset_modified`).

    ```mlir
    seaimp.reset_modified %mem[%p] : !seaimp.var<!m>
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead, MemWrite]>:$var,
                   SeaImp_Ptr:$addr);
  let assemblyFormat =
      "$var `[` $addr `]` attr-dict `:` qualified(type($var))";
  let hasVerifier = 1;
}

def SeaImp_PrintResource : Resource<"::sea::PrintResource">;

def SeaImp_PrintMemOp : SeaImp_Op<"print.mem", [
    SeaImp_IndexOf<"var", "addr">,
    MemoryEffects<[MemWrite<SeaImp_PrintResource>]>]> {
  let summary = "Show the contents of a region in a counterexample";
  let description = [{
    The strong `sea.print.mem` on the memory the variable holds: `get`,
    then print.mem with that version as its memory operand. It shows the
    region's contents at the print's point. `type`, `fmt`, `count`,
    `offset` and the optional count operand are those of `sea.print.mem`
    (docs/design/Print.md), written as keywords. The weak `sea.print.mem` names memories, not
    versions, and stays as it is in seaimp programs.

    ```mlir
    seaimp.print.mem "bal" %mem[%p] : !seaimp.var<!m> -> i64
    seaimp.print.mem "buf" %mem[%p], %n fmt "hexdump" : !seaimp.var<!m>, i64 -> i8
    seaimp.print.mem "v" %mem[%p] count 4 offset 8 : !seaimp.var<!m> -> i32
    ```
  }];
  let arguments = (ins Arg<SeaImp_MemVar, "", [MemRead]>:$var,
                   SeaImp_Index:$addr, Optional<SeaImp_BV>:$countValue,
                   StrAttr:$tag, TypeAttr:$type, OptionalAttr<StrAttr>:$fmt,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$count,
                   OptionalAttr<I64Attr>:$offset);
  let assemblyFormat = [{
    $tag $var `[` $addr `]` (`,` $countValue^)? (`fmt` $fmt^)?
    (`count` $count^)? (`offset` $offset^)? attr-dict `:`
    qualified(type($var)) (`,` type($countValue)^)? `->` $type
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Allocation in a region
//===----------------------------------------------------------------------===//

def SeaImp_AllocaOp : SeaImp_Op<"alloca", [Sea_NondetSiteOpInterface]> {
  let summary = "Allocate a new stack object in a region";
  let description = [{
    `sea.alloca` of an object whose cells are in the region `var`: each
    execution creates a new object of `size` units, aligned to `align`.
    The lowering is `sea.alloca` with the same operands and attributes
    (sea allocation takes no memory: pointers are not tied to a memory);
    the region is what a later partition into regions checks accesses
    against. `site` is unique with the program's other nondet sites; the
    result is a pointer that indexes the region.

    ```mlir
    %p = seaimp.alloca %mem, %c4 align 4 [1] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
    ```
  }];
  let arguments = (ins SeaImp_MemVar:$var, SeaImp_BV:$size,
                   ConfinedAttr<I64Attr, [IntPositive]>:$align,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site,
                   OptionalAttr<StrAttr>:$label);
  // no declared effects, as sea.alloca: an unused allocation stays (it
  // names an object of the program, by its site)
  let results = (outs SeaImp_Ptr:$result);
  let assemblyFormat = [{
    $var `,` $size `align` $align ` ` `[` $site `]` (`label` $label^)?
    attr-dict `:` qualified(type($var)) `,` type($size) `->`
    qualified(type($result))
  }];
  let hasVerifier = 1;
}

#endif // SEAIMP_OPS
