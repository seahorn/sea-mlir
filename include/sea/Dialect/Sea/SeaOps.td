// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaOps.td - sea dialect operations -----------------*- tablegen -*-===//

#ifndef SEA_OPS
#define SEA_OPS

include "sea/Dialect/Sea/SeaDialect.td"
include "sea/Dialect/Sea/SeaTypes.td"
include "mlir/IR/BuiltinAttributeInterfaces.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

defvar Sea_BV = AnySignlessInteger;
def Sea_Addr : Type<CPred<"::llvm::isa<::sea::AddrType>($_self)">, "address",
                    "::sea::AddrType">;
def Sea_Ptr : Type<CPred<"::llvm::isa<::sea::PtrType>($_self)">, "pointer",
                   "::sea::PtrType">;
def Sea_Mem : Type<CPred<"::llvm::isa<::sea::MemType>($_self)">, "memory",
                   "::sea::MemType">;
def Sea_Struct : Type<CPred<"::llvm::isa<::sea::StructType>($_self)">,
                      "struct", "::sea::StructType">;
// A value that fits in a register: bit-vector, address or pointer.
def Sea_BVOrAddr : AnyTypeOf<[Sea_BV, Sea_Addr, Sea_Ptr]>;
// A register value, structs included.
def Sea_RegValue : AnyTypeOf<[Sea_BV, Sea_Addr, Sea_Ptr, Sea_Struct]>;
// What indexes a memory.
def Sea_Index : AnyTypeOf<[Sea_Addr, Sea_Ptr]>;

// The index operand `addr` (an address or a pointer) indexes memory `mem`.
class Sea_AddrIndexes<string mem, string addr> : TypesMatchWith<
    "index type indexes the memory", mem, addr,
    "::llvm::cast<::sea::MemType>($_self).getIndex()">;
// The length operand `len` has the index width of memory `mem`.
class Sea_LenOf<string mem, string len> : TypesMatchWith<
    "length has the index width", mem, len,
    "::mlir::IntegerType::get($_self.getContext(), "
    "::llvm::cast<::sea::MemType>($_self).getIndexWidth())">;

//===----------------------------------------------------------------------===//
// Program
//===----------------------------------------------------------------------===//

def Sea_ProgramOp : Sea_Op<"program", [IsolatedFromAbove, SymbolName,
                                       SymbolVisibility, Symbol]> {
  let summary = "A verification program: one CFG";
  let description = [{
    A program is a CFG region. Execution starts at the entry block, which has
    no arguments, and ends normally at `sea.halt`. A failing `sea.assert` is
    an error; a failing `sea.assume` blocks the execution (it is infeasible).

    ```mlir
    sea.program @main {
      %x = sea.nondet "nondet_u32" [0] : i32
      %c = sea.const 10 : i32
      %lt = sea.icmp ult, %x, %c : i32
      sea.assume %lt
      sea.halt
    }
    ```
  }];
  let arguments = (ins SymbolNameAttr:$sym_name,
                   OptionalAttr<StrAttr>:$sym_visibility);
  let regions = (region AnyRegion:$body);
  let assemblyFormat =
      "($sym_visibility^)? $sym_name attr-dict-with-keyword $body";
  let hasRegionVerifier = 1;
}

def Sea_HaltOp : Sea_Op<"halt", [Terminator, HasParent<"ProgramOp">]> {
  let summary = "Normal end of a program";
  let assemblyFormat = "attr-dict";
}

def Sea_UnreachableOp : Sea_Op<"unreachable",
    [Terminator, HasParent<"ProgramOp">]> {
  let summary = "A block that cannot execute (LLVM unreachable)";
  let description = [{
    Reaching it means the execution is infeasible, like `sea.assume` of 0:
    the interpreter reports assume-blocked; the VC constrains the block not
    to execute.
  }];
  let assemblyFormat = "attr-dict";
}

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

def Sea_BrOp : Sea_Op<"br", [
    DeclareOpInterfaceMethods<BranchOpInterface>, Pure, Terminator]> {
  let summary = "Unconditional branch";
  let arguments = (ins Variadic<AnyType>:$destOperands);
  let successors = (successor AnySuccessor:$dest);
  let assemblyFormat = [{
    $dest (`(` $destOperands^ `:` type($destOperands) `)`)? attr-dict
  }];
}

def Sea_CondBrOp : Sea_Op<"cond_br", [AttrSizedOperandSegments,
    DeclareOpInterfaceMethods<BranchOpInterface>, Pure, Terminator]> {
  let summary = "Conditional branch on an i1";
  let description = [{
    Jumps to `trueDest` if the condition is 1, else to `falseDest`.
  }];
  let arguments = (ins I1:$condition, Variadic<AnyType>:$trueDestOperands,
                   Variadic<AnyType>:$falseDestOperands);
  let successors = (successor AnySuccessor:$trueDest, AnySuccessor:$falseDest);
  let assemblyFormat = [{
    $condition `,`
    $trueDest (`(` $trueDestOperands^ `:` type($trueDestOperands) `)`)? `,`
    $falseDest (`(` $falseDestOperands^ `:` type($falseDestOperands) `)`)?
    attr-dict
  }];
}

//===----------------------------------------------------------------------===//
// Verification and nondeterminism
//===----------------------------------------------------------------------===//

def Sea_AssumeOp : Sea_Op<"assume"> {
  let summary = "Block executions where the condition is 0";
  let arguments = (ins I1:$cond);
  let assemblyFormat = "$cond attr-dict";
}

def Sea_AssertOp : Sea_Op<"assert"> {
  let summary = "Fail executions where the condition is 0";
  let arguments = (ins I1:$cond);
  let assemblyFormat = "$cond attr-dict";
}

def Sea_NondetOp : Sea_Op<"nondet"> {
  let summary = "A nondeterministic bit-vector";
  let description = [{
    Exported 1:1 from a call to a nondet function (`nondet_u32()`, the `nd_*`
    family). `callee` is the original function name and `site` the call-site
    id, unique within the program. The site is the stable name of the value:
    the VC names its constant by it and replay looks it up in the model.

    ```mlir
    %x = sea.nondet "nondet_u32" [3] : i32
    ```
  }];
  let arguments = (ins StrAttr:$callee,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site);
  let results = (outs Sea_BV:$result);
  let assemblyFormat =
      "$callee ` ` `[` $site `]` attr-dict `:` type($result)";
}

//===----------------------------------------------------------------------===//
// Bit-vector operations
//===----------------------------------------------------------------------===//

// Folding: the pure value ops below (bit-vector ops, icmp, select, casts,
// concat / extract, intrinsics) fold when all operands are constants, to
// their value by evalPureOp (Dialect/Sea/Semantics.h): the folders never
// compute values themselves. Not folded: ops with nsw / nuw (promises),
// nondet, memory, address and pointer ops, structs. The dialect
// materializes folded integers as sea.const.

def Sea_ConstOp : Sea_Op<"const", [ConstantLike, Pure,
    AllTypesMatch<["value", "result"]>]> {
  let summary = "A bit-vector constant";
  let arguments = (ins TypedAttrInterface:$value);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$value attr-dict";
  let hasVerifier = 1;
  let hasFolder = 1;
}

class Sea_BinOp<string mnemonic, string smt> :
    Sea_Op<mnemonic, [Pure, SameOperandsAndResultType]> {
  let summary = "Bit-vector `" # smt # "`";
  let arguments = (ins Sea_BV:$lhs, Sea_BV:$rhs);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
  let hasFolder = 1;
}

// add, sub, mul and shl may carry LLVM's no-wrap flags as promises: `nsw`
// (the signed result fits), `nuw` (the unsigned result fits). The value is
// always the wrapped one; a flag only records the promise. The interpreter
// warns when one is broken; `sea-wrap-flags` turns them into sea.assume /
// sea.assert (or drops them); the VC ignores them (as SeaHorn does).
class Sea_WrapBinOp<string mnemonic, string smt> :
    Sea_Op<mnemonic, [Pure, SameOperandsAndResultType]> {
  let summary = "Bit-vector `" # smt # "`, optionally with no-wrap promises";
  let arguments = (ins Sea_BV:$lhs, Sea_BV:$rhs, UnitAttr:$nsw,
                   UnitAttr:$nuw);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = [{
    $lhs `,` $rhs (`nsw` $nsw^)? (`nuw` $nuw^)? attr-dict `:` type($result)
  }];
  // Folds only without flags: folding would drop a promise the interpreter
  // checks (and sea-wrap-flags turns into an assume / assert).
  let hasFolder = 1;
}

def Sea_AddOp  : Sea_WrapBinOp<"add",  "bvadd">;
def Sea_SubOp  : Sea_WrapBinOp<"sub",  "bvsub">;
def Sea_MulOp  : Sea_WrapBinOp<"mul",  "bvmul">;
def Sea_UDivOp : Sea_BinOp<"udiv", "bvudiv">;
def Sea_SDivOp : Sea_BinOp<"sdiv", "bvsdiv">;
def Sea_URemOp : Sea_BinOp<"urem", "bvurem">;
def Sea_SRemOp : Sea_BinOp<"srem", "bvsrem">;
def Sea_AndOp  : Sea_BinOp<"and",  "bvand">;
def Sea_OrOp   : Sea_BinOp<"or",   "bvor">;
def Sea_XorOp  : Sea_BinOp<"xor",  "bvxor">;
def Sea_ShlOp  : Sea_WrapBinOp<"shl",  "bvshl">;
def Sea_LShrOp : Sea_BinOp<"lshr", "bvlshr">;
def Sea_AShrOp : Sea_BinOp<"ashr", "bvashr">;

def Sea_ICmpPredicateAttr : I64EnumAttr<"ICmpPredicate", "", [
    I64EnumAttrCase<"eq", 0>,
    I64EnumAttrCase<"ne", 1>,
    I64EnumAttrCase<"slt", 2>,
    I64EnumAttrCase<"sle", 3>,
    I64EnumAttrCase<"sgt", 4>,
    I64EnumAttrCase<"sge", 5>,
    I64EnumAttrCase<"ult", 6>,
    I64EnumAttrCase<"ule", 7>,
    I64EnumAttrCase<"ugt", 8>,
    I64EnumAttrCase<"uge", 9>,
  ]> {
  let cppNamespace = "::sea";
}

def Sea_ICmpOp : Sea_Op<"icmp", [Pure, SameTypeOperands]> {
  let summary = "Bit-vector comparison; 1 if it holds, else 0";
  let arguments = (ins Sea_ICmpPredicateAttr:$predicate, Sea_BV:$lhs,
                   Sea_BV:$rhs);
  let results = (outs I1:$result);
  let assemblyFormat =
      "$predicate `,` $lhs `,` $rhs attr-dict `:` type($lhs)";
  let hasFolder = 1;
}

def Sea_SelectOp : Sea_Op<"select", [Pure,
    AllTypesMatch<["trueValue", "falseValue", "result"]>]> {
  let summary = "`trueValue` if the condition is 1, else `falseValue`";
  let arguments = (ins I1:$condition, Sea_RegValue:$trueValue,
                   Sea_RegValue:$falseValue);
  let results = (outs Sea_RegValue:$result);
  let assemblyFormat = [{
    $condition `,` $trueValue `,` $falseValue attr-dict `:` type($result)
  }];
  // Folds a constant condition (any type) or equal arms.
  let hasFolder = 1;
}

class Sea_CastOp<string mnemonic, string desc> : Sea_Op<mnemonic, [Pure]> {
  let summary = desc;
  let arguments = (ins Sea_BV:$input);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$input attr-dict `:` type($input) `to` type($result)";
  let hasVerifier = 1;
  let hasFolder = 1;
}

def Sea_ConcatOp : Sea_Op<"concat", [Pure]> {
  let summary = "SMT concat: `hi` above `lo`";
  let description = [{
    The result has width(hi) + width(lo) bits; `lo` is the low part.

    ```mlir
    %w = sea.concat %hi, %lo : i8, i8 -> i16
    ```
  }];
  let arguments = (ins Sea_BV:$hi, Sea_BV:$lo);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = [{
    $hi `,` $lo attr-dict `:` type($hi) `,` type($lo) `->` type($result)
  }];
  let hasVerifier = 1;
  let hasFolder = 1;
}

def Sea_ExtractOp : Sea_Op<"extract", [Pure]> {
  let summary = "SMT extract: the result's width of bits from bit `low`";
  let description = [{
    ```mlir
    %b1 = sea.extract %w[8] : i16 -> i8   // bits 8..15
    ```
  }];
  let arguments = (ins Sea_BV:$input,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$low);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = [{
    $input `[` $low `]` attr-dict `:` type($input) `->` type($result)
  }];
  let hasVerifier = 1;
  let hasFolder = 1;
}

def Sea_ZExtOp  : Sea_CastOp<"zext", "Zero extension to a wider bit-vector">;
def Sea_SExtOp  : Sea_CastOp<"sext", "Sign extension to a wider bit-vector">;
def Sea_TruncOp : Sea_CastOp<"trunc", "Keep the low bits (narrower result)">;

//===----------------------------------------------------------------------===//
// Intrinsics: LLVM intrinsics as logical ops. The interpreter executes them
// directly; `sea-expand-intrinsics` rewrites them into the ops above for the
// VC, which refuses them.
//===----------------------------------------------------------------------===//

def Sea_UMinOp : Sea_BinOp<"umin", "unsigned minimum">;
def Sea_UMaxOp : Sea_BinOp<"umax", "unsigned maximum">;
def Sea_SMinOp : Sea_BinOp<"smin", "signed minimum">;
def Sea_SMaxOp : Sea_BinOp<"smax", "signed maximum">;

def Sea_BSwapOp : Sea_Op<"bswap", [Pure, SameOperandsAndResultType]> {
  let summary = "Reverse the bytes (llvm.bswap); the width is a multiple of 16";
  let arguments = (ins Sea_BV:$input);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$input attr-dict `:` type($result)";
  let hasVerifier = 1;
  let hasFolder = 1;
}

def Sea_CtPopOp : Sea_Op<"ctpop", [Pure, SameOperandsAndResultType]> {
  let summary = "The number of 1 bits (llvm.ctpop)";
  let arguments = (ins Sea_BV:$input);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$input attr-dict `:` type($result)";
  let hasFolder = 1;
}

class Sea_FunnelShiftOp<string mnemonic, string desc> :
    Sea_Op<mnemonic, [Pure, SameOperandsAndResultType]> {
  let summary = desc;
  let description = [{
    The shift amount is taken modulo the width (llvm.fshl / llvm.fshr):
    `fshl` shifts `hi:lo` left and keeps the high half; `fshr` shifts it
    right and keeps the low half.
  }];
  let arguments = (ins Sea_BV:$hi, Sea_BV:$lo, Sea_BV:$shift);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$hi `,` $lo `,` $shift attr-dict `:` type($result)";
  let hasFolder = 1;
}

def Sea_FShlOp : Sea_FunnelShiftOp<"fshl", "Funnel shift left">;
def Sea_FShrOp : Sea_FunnelShiftOp<"fshr", "Funnel shift right">;

// Overflow predicates, named and defined as in SMT-LIB 2.7 (bvuaddo, ...).
// Add, sub, neg and div overflow are always expanded by
// sea-expand-intrinsics (solvers expand them eagerly anyway, so one
// expansion serves every solver); mul overflow is encoded natively when the
// solver has it (z3) and expanded otherwise.
class Sea_OverflowOp<string mnemonic, string desc> :
    Sea_Op<mnemonic, [Pure, SameTypeOperands]> {
  let summary = "1 iff " # desc # " overflows (SMT-LIB bv" # mnemonic # ")";
  let description = [{
    1 iff the result of the operation on unbounded integers (unsigned or
    signed reading of the operands) is not representable in the operand
    width. llvm.*.with.overflow is the plain `sea.add` / `sea.sub` /
    `sea.mul` for the value and one of these for the bit.
  }];
  let arguments = (ins Sea_BV:$lhs, Sea_BV:$rhs);
  let results = (outs I1:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($lhs)";
  let hasFolder = 1;
}

def Sea_UAddOOp : Sea_OverflowOp<"uaddo", "unsigned addition">;
def Sea_SAddOOp : Sea_OverflowOp<"saddo", "signed addition">;
def Sea_USubOOp : Sea_OverflowOp<"usubo", "unsigned subtraction">;
def Sea_SSubOOp : Sea_OverflowOp<"ssubo", "signed subtraction">;
def Sea_UMulOOp : Sea_OverflowOp<"umulo", "unsigned multiplication">;
def Sea_SMulOOp : Sea_OverflowOp<"smulo", "signed multiplication">;
def Sea_SDivOOp : Sea_OverflowOp<"sdivo", "signed division">;

def Sea_NegOOp : Sea_Op<"nego", [Pure]> {
  let summary = "1 iff signed negation overflows (SMT-LIB bvnego)";
  let description = "1 iff the input is the minimal signed value.";
  let arguments = (ins Sea_BV:$input);
  let results = (outs I1:$result);
  let assemblyFormat = "$input attr-dict `:` type($input)";
  let hasFolder = 1;
}

//===----------------------------------------------------------------------===//
// Structs (register aggregates; sea-scalarize-structs lowers them)
//===----------------------------------------------------------------------===//

def Sea_StructCreateOp : Sea_Op<"struct.create", [Pure]> {
  let summary = "A struct from its fields";
  let description = [{
    ```mlir
    %s = sea.struct.create %a, %b : i16, i32 -> !sea.struct<i16, i32>
    ```
  }];
  let arguments = (ins Variadic<Sea_RegValue>:$fields);
  let results = (outs Sea_Struct:$result);
  let assemblyFormat = [{
    $fields attr-dict `:` qualified(type($fields)) `->` qualified(type($result))
  }];
  let hasVerifier = 1;
}

def Sea_StructNondetOp : Sea_Op<"struct.nondet"> {
  let summary = "A struct with nondeterministic fields";
  let description = [{
    The explicit nondet initialization of a struct (LLVM's `insertvalue
    undef, ...` idiom maps here: sea has no undef). Its value is one
    nondeterministic bit-vector, the concatenation of the struct's integer
    and address leaves in leaf order (the first leaf in the low bits); its
    pointer leaves are null (no nondet pointers before placement). `site`
    names that bit-vector as for `sea.nondet`: the replay input `site=v` and
    the VC constant `nd.<site>`.

    ```mlir
    %s = sea.struct.nondet "undef" [4] : !sea.struct<i16, i32>
    ```
  }];
  let arguments = (ins StrAttr:$callee,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site);
  let results = (outs Sea_Struct:$result);
  let assemblyFormat =
      "$callee ` ` `[` $site `]` attr-dict `:` qualified(type($result))";
  let extraClassDeclaration = [{
    /// Width of the nondet bit-vector: the sum of the widths of the
    /// integer and address leaves (0 if there are none).
    unsigned getBitsWidth();
  }];
}

def Sea_StructExtractOp : Sea_Op<"struct.extract", [Pure]> {
  let summary = "Field `index` of a struct";
  let description = [{
    ```mlir
    %b = sea.struct.extract %s[1] : !sea.struct<i16, i32> -> i32
    ```
  }];
  let arguments = (ins Sea_Struct:$input,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$index);
  let results = (outs Sea_RegValue:$result);
  let assemblyFormat = [{
    $input `[` $index `]` attr-dict `:` qualified(type($input)) `->`
    qualified(type($result))
  }];
  let hasVerifier = 1;
}

def Sea_StructInsertOp : Sea_Op<"struct.insert", [Pure,
    AllTypesMatch<["input", "result"]>]> {
  let summary = "A struct with field `index` replaced";
  let description = [{
    ```mlir
    %t = sea.struct.insert %v, %s[1] : i32 into !sea.struct<i16, i32>
    ```
  }];
  let arguments = (ins Sea_RegValue:$value, Sea_Struct:$input,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$index);
  let results = (outs Sea_Struct:$result);
  let assemblyFormat = [{
    $value `,` $input `[` $index `]` attr-dict `:` qualified(type($value))
    `into` qualified(type($input))
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Addresses
//===----------------------------------------------------------------------===//

def Sea_AddrAddOp : Sea_Op<"addr.add", [Pure,
    AllTypesMatch<["addr", "result"]>]> {
  let summary = "Address plus an offset in address units (mod 2^width)";
  let arguments = (ins Sea_Addr:$addr, Sea_BV:$offset);
  let results = (outs Sea_Addr:$result);
  let assemblyFormat =
      "$addr `,` $offset attr-dict `:` qualified(type($addr)) `,` qualified(type($offset))";
  let hasVerifier = 1;
}

def Sea_AddrDiffOp : Sea_Op<"addr.diff", [Pure, SameTypeOperands]> {
  let summary = "Difference of two addresses in units (mod 2^width)";
  let arguments = (ins Sea_Addr:$lhs, Sea_Addr:$rhs);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` qualified(type($lhs)) `->` qualified(type($result))";
  let hasVerifier = 1;
}

def Sea_AddrCmpOp : Sea_Op<"addr.cmp", [Pure, SameTypeOperands]> {
  let summary = "Address comparison; 1 if it holds, else 0";
  let arguments = (ins Sea_ICmpPredicateAttr:$predicate, Sea_Addr:$lhs,
                   Sea_Addr:$rhs);
  let results = (outs I1:$result);
  let assemblyFormat =
      "$predicate `,` $lhs `,` $rhs attr-dict `:` qualified(type($lhs))";
}

def Sea_AddrToIntOp : Sea_Op<"addr.to_int", [Pure]> {
  let summary = "The bits of an address (an escape: ptrtoint)";
  let arguments = (ins Sea_Addr:$addr);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$addr attr-dict `:` qualified(type($addr)) `to` qualified(type($result))";
  let hasVerifier = 1;
}

def Sea_AddrFromIntOp : Sea_Op<"addr.from_int", [Pure]> {
  let summary = "The address with the given bits (an escape: inttoptr)";
  let arguments = (ins Sea_BV:$value);
  let results = (outs Sea_Addr:$result);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value)) `to` qualified(type($result))";
  let hasVerifier = 1;
}

def Sea_AddrFromBitsOp : Sea_Op<"addr.from_bits", [Pure]> {
  let summary = "An address reassembled from its own stored bits";
  let description = [{
    The address with the given bits, as addr.from_int, but not an escape:
    the bits are the address's own, stored and read back cell by cell
    (sea-split-wide-access reassembles a pointer load so). Its metadata
    (offset, size: the fat-pointer companions) is in the companion
    memories at the load's address, so the pointer is reconstructed, not
    made from an integer (docs/design/PtrSort.md, D2). So
    it does not make a program address mode (--ptr-mode).
  }];
  let arguments = (ins Sea_BV:$value);
  let results = (outs Sea_Addr:$result);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value)) `to` qualified(type($result))";
  let hasVerifier = 1;
}

def Sea_AddrToBitsOp : Sea_Op<"addr.to_bits", [Pure]> {
  let summary = "The bits of an address, to be stored cell by cell";
  let description = [{
    The bits of an address, as addr.to_int, but not an escape: the value
    goes to memory, split into cells, and its metadata to the companion
    memories at the store's address (sea-split-wide-access splits a pointer
    store so). The inverse of addr.from_bits.
  }];
  let arguments = (ins Sea_Addr:$addr);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = "$addr attr-dict `:` qualified(type($addr)) `to` qualified(type($result))";
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Memory (see docs/design/Memory.md)
//===----------------------------------------------------------------------===//

def Sea_MemConstOp : Sea_Op<"mem.const", [
    TypesMatchWith<"value is a cell", "result", "value",
                   "::llvm::cast<::sea::MemType>($_self).getCell()">]> {
  let summary = "A memory whose every cell holds `value`";
  let description = [{
    Shadow memories start this way (an empty set of modified objects is an
    all-zero shadow).

    `shadow` marks a memory a tool made for metadata keyed by object
    identity (sea-lower-tracking: one cell per object base), not program
    data: no value depends on it (the interpreter ignores it); the VC (SeaM)
    reads every memory that starts here as a shadow memory and applies no
    A-INB rule to its accesses (docs/design/SeaM-null.md, "Shadow memories").

    Not Pure: the result is an allocation (a new buffer of the in-place
    interpreter, the head of its own Mem-SSA chain). Two mem.const ops with
    the same value must stay two memories: merged (cse), both chains would
    be consumed `inplace` from one buffer. Unused, it is trivially dead.

    ```mlir
    %s = sea.mem.const %zero shadow : !sea.mem<!sea.ptr<64>, i8>
    ```
  }];
  let arguments = (ins Sea_BV:$value, UnitAttr:$shadow);
  let results = (outs Res<Sea_Mem, "", [MemAlloc<DefaultResource>]>:$result);
  let assemblyFormat =
      "$value (`shadow` $shadow^)? attr-dict `:` qualified(type($result))";
}

def Sea_MemNondetOp : Sea_Op<"mem.nondet"> {
  let summary = "Unconstrained initial contents of a memory region";
  let description = [{
    `site` is the replay key, unique per program together with the sites of
    `sea.nondet` and `sea.memhavoc`.

    ```mlir
    %m = sea.mem.nondet "heap" [4] : !sea.mem<!sea.addr<64>, i8>
    ```
  }];
  let arguments = (ins StrAttr:$name,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site);
  let results = (outs Sea_Mem:$result);
  let assemblyFormat =
      "$name ` ` `[` $site `]` attr-dict `:` qualified(type($result))";
}

def Sea_LoadOp : Sea_Op<"load", [Pure, Sea_AddrIndexes<"mem", "addr">]> {
  let summary = "Read the cell at an address";
  let description = [{
    The result is an integer, address or pointer whose width is a multiple k
    of the cell width: it is made of the k cells from `addr` on,
    little-endian (the cell at `addr` holds the low bits). Reading an
    earlier version is allowed (see Memory.md).

    `align` (here and on store, memset, memmove; memmove also `src_align`)
    is LLVM's: a promise, in bytes, that the address is a multiple of it.
    No op's value depends on it; the interpreter reports a broken one
    (interp.align-promise); a pass may drop it.
  }];
  let arguments = (ins Sea_Mem:$mem, Sea_Index:$addr,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align);
  let results = (outs Sea_BVOrAddr:$result);
  let assemblyFormat =
      "$mem `[` $addr `]` (`align` $align^)? attr-dict `:` qualified(type($mem)) `->` qualified(type($result))";
  let hasVerifier = 1;
}

class Sea_MemUpdateOp<string mnemonic, list<Trait> traits = []> :
    Sea_Op<mnemonic, !listconcat([AllTypesMatch<["mem", "result"]>,
                                  Sea_AddrIndexes<"mem", "addr">], traits)> {
  let results = (outs Sea_Mem:$result);
}

def Sea_StoreOp : Sea_MemUpdateOp<"store"> {
  let summary = "Write a value: m[addr + i := cell i of value]";
  let description = [{
    The value's width is a multiple k of the cell width; it is written to
    the k cells from `addr` on, little-endian. Returns a new version. With
    `inplace`, promises that `mem` is dead
    afterwards, so the new version may reuse its storage (the interpreter
    accepts only in-place memory updates).
  }];
  let arguments = (ins Sea_Mem:$mem, Sea_Index:$addr, Sea_BVOrAddr:$value,
                   UnitAttr:$inplace, OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $mem `[` $addr `]` `,` $value (`inplace` $inplace^)? (`align` $align^)?
    (`tracked` $tracked^)? attr-dict `:`
    qualified(type($mem)) `,` qualified(type($value))
  }];
  let hasVerifier = 1;
}

def Sea_MemSetOp : Sea_MemUpdateOp<"memset", [
    Sea_LenOf<"mem", "len">,
    TypesMatchWith<"value is one index unit", "mem", "value",
                   "::llvm::cast<::sea::MemType>($_self).getUnitType()">]> {
  let summary = "Set `len` index units from `addr` to a unit value";
  let description = [{
    `value` is one index unit (an i8 for byte-addressed memory), as LLVM's
    memset; in a word layout each k-byte cell becomes k copies of it, and
    `len` (bytes) must cover whole cells.
  }];
  let arguments = (ins Sea_Mem:$mem, Sea_Index:$addr, Sea_BV:$value,
                   Sea_BV:$len, UnitAttr:$inplace, OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $mem `[` $addr `]` `,` $value `,` $len (`inplace` $inplace^)?
    (`align` $align^)? (`tracked` $tracked^)? attr-dict
    `:` qualified(type($mem))
  }];
  let hasVerifier = 1;
}

def Sea_MemMoveOp : Sea_MemUpdateOp<"memmove", [
    AllTypesMatch<["mem", "src"]>, Sea_AddrIndexes<"mem", "srcAddr">,
    Sea_LenOf<"mem", "len">]> {
  let summary = "Copy `len` cells from `src` at `srcAddr` to `mem` at `addr`";
  let description = [{
    Reads `src` as it was before the op, so overlap is harmless; `src` may
    be the same version as `mem` or another region of the same type.
  }];
  let arguments = (ins Sea_Mem:$mem, Sea_Index:$addr, Sea_Mem:$src,
                   Sea_Index:$srcAddr, Sea_BV:$len, UnitAttr:$inplace,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$align, OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$src_align,
                   UnitAttr:$tracked);
  let assemblyFormat = [{
    $mem `[` $addr `]` `,` $src `[` $srcAddr `]` `,` $len
    (`inplace` $inplace^)? (`align` $align^)? (`src_align` $src_align^)?
    (`tracked` $tracked^)? attr-dict `:` qualified(type($mem))
  }];
  let hasVerifier = 1;
}

def Sea_MemHavocOp : Sea_MemUpdateOp<"memhavoc", [Sea_LenOf<"mem", "len">]> {
  let summary = "Make `len` cells from `addr` nondeterministic";
  let description = [{
    Only the range changes. `site` is the replay key (shared with the other
    nondet sites).
  }];
  let arguments = (ins Sea_Mem:$mem, Sea_Index:$addr, Sea_BV:$len,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site,
                   UnitAttr:$inplace, UnitAttr:$tracked);
  let assemblyFormat = [{
    $mem `[` $addr `]` `,` $len ` ` `[` $site `]` (`inplace` $inplace^)?
    (`tracked` $tracked^)? attr-dict `:` qualified(type($mem))
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Modification tracking (abstract level). A memory value is its contents
// plus the set of objects that `tracked` updates wrote to (initially
// empty). The tracked region is static: sea.tracking_on / tracking_off
// markers sit at positions every execution passes (they dominate the
// exit), and sea-apply-tracking turns them into `tracked` on the updates
// between them (an update in that region: store, memset, memmove,
// memhavoc).
//===----------------------------------------------------------------------===//

def Sea_IsModifiedOp : Sea_Op<"mem.is_modified", [Pure,
    Sea_AddrIndexes<"mem", "ptr">]> {
  let summary = "1 if a tracked update wrote to `ptr`'s object";
  let description = [{
    A property of the object (as SeaHorn's sea_is_modified): any byte of it,
    since the start of tracking or its last sea.mem.reset_modified.
  }];
  let arguments = (ins Sea_Mem:$mem, Sea_Ptr:$ptr);
  let results = (outs I1:$result);
  let assemblyFormat =
      "$mem `[` $ptr `]` attr-dict `:` qualified(type($mem))";
}

def Sea_ResetModifiedOp : Sea_MemUpdateOp<"mem.reset_modified"> {
  let summary = "Forget that `ptr`'s object was modified";
  let arguments = (ins Sea_Mem:$mem, Sea_Ptr:$addr, UnitAttr:$inplace);
  let assemblyFormat = [{
    $mem `[` $addr `]` (`inplace` $inplace^)? attr-dict `:`
    qualified(type($mem))
  }];
}

def Sea_TrackingOnOp : Sea_Op<"tracking_on"> {
  let summary = "Marker: tracked updates start here (sea-apply-tracking)";
  let assemblyFormat = "attr-dict";
}

def Sea_TrackingOffOp : Sea_Op<"tracking_off"> {
  let summary = "Marker: tracked updates stop here (sea-apply-tracking)";
  let assemblyFormat = "attr-dict";
}


//===----------------------------------------------------------------------===//
// Abstract pointers and allocation (docs/design/Memory-Step2.md)
//===----------------------------------------------------------------------===//

class Sea_AllocOp<string mnemonic, string what> : Sea_Op<mnemonic> {
  let summary = "Allocate a new " # what # " object";
  let description = [{
    Each execution creates a new object of `size` units, aligned to
    `align`. `site` is unique per program (shared with the nondet sites)
    and names the object in the segment map and in traces. `label` (a
    producer's name for the object: a source variable, "malloc at
    foo.c:12") is shown in the counterexample object table
    (docs/design/Print.md); sea-place-static copies it to the segment.
  }];
  let arguments = (ins Sea_BV:$size,
                   ConfinedAttr<I64Attr, [IntPositive]>:$align,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site,
                   OptionalAttr<StrAttr>:$label);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = [{
    $size `align` $align ` ` `[` $site `]` (`label` $label^)? attr-dict `:` type($size) `->`
    qualified(type($result))
  }];
  let hasVerifier = 1;
}

def Sea_AllocaOp : Sea_AllocOp<"alloca", "stack">;
def Sea_MallocOp : Sea_AllocOp<"malloc", "heap">;

def Sea_GlobalOp : Sea_Op<"global", [Pure]> {
  let summary = "The object of a global variable";
  let description = [{
    Every use of the same `name` denotes the same object; `size` and `align`
    must agree between uses.

    ```mlir
    %g = sea.global @counter size 4 align 4 : !sea.ptr<64>
    ```
  }];
  let arguments = (ins FlatSymbolRefAttr:$name,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$size,
                   ConfinedAttr<I64Attr, [IntPositive]>:$align,
                   OptionalAttr<StrAttr>:$label);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = [{
    $name `size` $size `align` $align (`label` $label^)? attr-dict `:` qualified(type($result))
  }];
}

def Sea_FuncAddrOp : Sea_Op<"func_addr", [Pure]> {
  let summary = "The address of a function (a size-0 object)";
  let description = [{
    Unique per function, comparable and storable.
  }];
  let arguments = (ins FlatSymbolRefAttr:$name);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = "$name attr-dict `:` qualified(type($result))";
}

def Sea_FreeOp : Sea_Op<"free"> {
  let summary = "End the lifetime of a heap object";
  let arguments = (ins Sea_Ptr:$ptr);
  let assemblyFormat = "$ptr attr-dict `:` qualified(type($ptr))";
}

def Sea_PtrAddOp : Sea_Op<"ptr.add", [Pure,
    AllTypesMatch<["ptr", "result"]>]> {
  let summary = "Pointer plus an offset in units (same object)";
  let arguments = (ins Sea_Ptr:$ptr, Sea_BV:$offset);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = [{
    $ptr `,` $offset attr-dict `:` qualified(type($ptr)) `,` type($offset)
  }];
  let hasVerifier = 1;
}

def Sea_PtrIndexOp : Sea_Op<"ptr.index", [Pure,
    AllTypesMatch<["base", "result"]>]> {
  let summary = "GEP-style pointer arithmetic: base + sum sext(i_k)*s_k + c";
  let description = [{
    `%q = sea.ptr.index %p [%i, %j] strides [24, -4] offset 80 inbounds
          : !sea.ptr<64>, i64, i32`

    The value (total, modular; the flags never change it):
    `q = p + sext(i_1)*s_1 + ... + sext(i_m)*s_m + c (mod 2^n)`, n the
    base's width. The base is a pointer (`!sea.ptr`) or, after placement, an
    address (`!sea.addr`). An index narrower than n is sign-extended (GEP).
    The strides and the offset c are n-bit two's complement values, written
    as their sign-extension to 64 bits (one spelling per value). Pointer
    arithmetic is explicit (docs/design/PtrIndex.md): a consumer reads the
    structure from the op, never rediscovers it from integer arithmetic.

    Flags: trusted front-end promises, LLVM's GEP no-wrap flags, in their
    order-free form (PtrIndex.md section 1; `inbounds` implies `nusw`):
    - P1: each product `sext(i_k) * s_k` fits in n signed bits;
    - P2: the total offset O = sum + c fits in n signed bits;
    - P3: `p + O` (unsigned plus signed) is in [0, 2^n), no wrap above or
      below zero.
    `nuw`: P1-P3 with unsigned products, offset and sum. Like `align`, no
    value depends on them; the interpreter warns
    (`interp.ptr-index-promise`) when one is broken, a pass may drop them.
    In LLVM a broken flag makes the GEP poison, UB only when the pointer is
    used: the promise is a fact at an access, not at the op.

    Every stride is nonzero. The index list may be empty: a constant-only
    GEP with a promise (`&t->count` inbounds, PtrIndex.md rev 7, A5); without
    a flag, a constant-only GEP is `sea.ptr.add` / `sea.addr.add`.
  }];
  let arguments = (ins Sea_Index:$base, Variadic<Sea_BV>:$indices,
                   DenseI64ArrayAttr:$strides, I64Attr:$offset,
                   UnitAttr:$inbounds, UnitAttr:$nusw, UnitAttr:$nuw);
  let results = (outs Sea_Index:$result);
  let assemblyFormat = [{
    $base `[` $indices `]` `strides` $strides `offset` $offset
    (`inbounds` $inbounds^)? (`nusw` $nusw^)? (`nuw` $nuw^)?
    attr-dict `:` qualified(type($base)) (`,` type($indices)^)?
  }];
  let hasVerifier = 1;
}

def Sea_PtrDiffOp : Sea_Op<"ptr.diff", [Pure, SameTypeOperands]> {
  let summary = "Offset difference of two pointers into the same object";
  let arguments = (ins Sea_Ptr:$lhs, Sea_Ptr:$rhs);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = [{
    $lhs `,` $rhs attr-dict `:` qualified(type($lhs)) `->` type($result)
  }];
  let hasVerifier = 1;
}

def Sea_PtrCmpOp : Sea_Op<"ptr.cmp", [Pure, SameTypeOperands]> {
  let summary = "Pointer comparison; 1 if it holds, else 0";
  let description = [{
    `eq` and `ne` compare any pointers; ordered predicates compare the
    addresses (ADDR in the VC, after placement) of pointers into the same
    object (else UB; the interpreter stops). Before placement the
    interpreter orders them by offset, as any placement that keeps the
    object off address 0 and 2^(w-1) does (offsets read as signed;
    outside the object it warns `interp.ptr-cmp-outside`).
  }];
  let arguments = (ins Sea_ICmpPredicateAttr:$predicate, Sea_Ptr:$lhs,
                   Sea_Ptr:$rhs);
  let results = (outs I1:$result);
  let assemblyFormat = [{
    $predicate `,` $lhs `,` $rhs attr-dict `:` qualified(type($lhs))
  }];
}

def Sea_PtrBaseOp : Sea_Op<"ptr.base", [Pure, SameOperandsAndResultType]> {
  let summary = "The pointer to the start of a pointer's object";
  let description = [{
    Same object, offset 0 (`to_base_ptr`); the base of null is null.
    Per-object metadata is keyed by it: `is_modified` of an object is the
    query at `sea.ptr.base %p`.
  }];
  let arguments = (ins Sea_Ptr:$ptr);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = "$ptr attr-dict `:` qualified(type($ptr))";
}

def Sea_PtrDereferenceableOp : Sea_Op<"ptr.dereferenceable", [Pure]> {
  let summary = "1 if the `len` bytes from `ptr` lie in its object";
  let description = [{
    Spatial: `ptr` is not null and offset <= size and len <= size - offset
    (no wrap), with the size of `ptr`'s object; whether the object is still
    alive is not part of it (temporal safety is separate). SeaHorn's
    `sea.is_dereferenceable` (fat pointers: 0 <= offset and offset + len <=
    size). `len` has the pointer width.
  }];
  let arguments = (ins Sea_Ptr:$ptr, Sea_BV:$len);
  let results = (outs I1:$result);
  let assemblyFormat = [{
    $ptr `,` $len attr-dict `:` qualified(type($ptr)) `,` type($len)
  }];
  let hasVerifier = 1;
}

def Sea_PtrNondetOp : Sea_Op<"ptr.nondet"> {
  let summary = "A pointer value that can never be dereferenced";
  let description = [{
    Each execution makes a fresh opaque object of size 0 (named
    nondet.<site>): the pointer is not null, equals no other object's
    pointers, and no access through it is in bounds
    (sea.ptr.dereferenceable is 0). Harnesses use it to mark fields that
    must never be dereferenced (SeaHorn's nd_* functions returning
    pointers). `site` is unique with the other nondet sites.
  }];
  let arguments = (ins StrAttr:$name,
                   ConfinedAttr<I64Attr, [IntNonNegative]>:$site);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat =
      "$name ` ` `[` $site `]` attr-dict `:` qualified(type($result))";
}

def Sea_PtrNullOp : Sea_Op<"ptr.null", [Pure]> {
  let summary = "The null pointer";
  let description = [{
    A pointer into the null object (size 0). Placement makes it the result of
    the segment "null" (base 0), so the VC with SeaM sees the base obj.null
    (docs/design/SeaM-null.md). This op is the only source of null
    provenance: `sea.ptr.from_int 0` is the same address but stays a number
    after placement.
  }];
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = "attr-dict `:` qualified(type($result))";
}

def Sea_PtrToIntOp : Sea_Op<"ptr.to_int", [Pure]> {
  let summary = "The address of a pointer (an escape: ptrtoint)";
  let description = [{
    Has no value before placement: the abstract interpreter stops here.
  }];
  let arguments = (ins Sea_Ptr:$ptr);
  let results = (outs Sea_BV:$result);
  let assemblyFormat = [{
    $ptr attr-dict `:` qualified(type($ptr)) `to` type($result)
  }];
  let hasVerifier = 1;
}

def Sea_PtrFromIntOp : Sea_Op<"ptr.from_int", [Pure]> {
  let summary = "The pointer with the given address (an escape: inttoptr)";
  let description = [{
    Before placement only 0 (null) has a meaning: the abstract interpreter
    stops on any other value.
  }];
  let arguments = (ins Sea_BV:$value);
  let results = (outs Sea_Ptr:$result);
  let assemblyFormat = [{
    $value attr-dict `:` type($value) `to` qualified(type($result))
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Placement output
//===----------------------------------------------------------------------===//

def Sea_SegmentOp : Sea_Op<"segment", [Pure]> {
  let summary = "Where placement put an object: its base address";
  let description = [{
    Emitted by `sea-place-static` at the start of the program's entry block,
    one per placed object. The result is the object's base address: placement
    replaces the uses of an object by this result (plus `sea.addr.add` for
    offsets), so the object a pointer was derived from stays visible after
    placement (SeaM, docs/design/SeaM.md). `base` and `size` are in address
    units of the program's address type; `kind` is stack, heap, global,
    func or null (`label`: the allocation's, if it had one); the name is `<kind>.<site>`, or `<kind>.<name>` for globals
    and functions; the null object (docs/design/SeaM-null.md) is the segment
    "null", base 0, size 0, kind null, whose result replaces sea.ptr.null. The program is the same one with the numeral `base` in its
    place: the interpreter evaluates the op to `base`, and the VC encodes it
    as that numeral (as a named constant `obj.<name>` with a side equality
    when SeaM is on).

    The op has no folder, on purpose: constant propagation (sccp) must not
    replace the result by a numeral, or the provenance is lost.

    ```mlir
    %b = sea.segment "stack.3" base 3221225456 size 16 align 8 kind "stack"
        : !sea.addr<64>
    ```
  }];
  let arguments = (ins StrAttr:$name, APIntAttr:$base, APIntAttr:$size,
                   ConfinedAttr<I64Attr, [IntPositive]>:$align,
                   StrAttr:$kind, OptionalAttr<StrAttr>:$label);
  let results = (outs Sea_Addr:$result);
  let assemblyFormat = [{
    $name `base` $base `size` $size `align` $align `kind` $kind
    (`label` $label^)? attr-dict
    `:` qualified(type($result))
  }];
  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// Counterexample printing (docs/design/Print.md)
//
// A print shows values in a counterexample (sea-run, sea-vc replay) and has
// no other effect: the interpreter only renders, the VC encodes nothing.
// Two forms (ctac's weak_refs / strong_refs):
//   - weak: the print names what it shows (string attributes); the names
//     are not uses, so they keep nothing alive and no pass has to know the
//     print. A name is `nd.<site>` (the value of the sea.nondet /
//     sea.struct.nondet / sea.ptr.nondet at that site, or the memory region
//     of the sea.mem.nondet / sea.memhavoc at that site), or else the name
//     of a NameLoc (`loc("x")`, `loc("x"(...))`) on a value: an op with one
//     result or a block argument, the name `sea-opt
//     --mlir-use-nameloc-as-prefix` prints as `%x`. The interpreter peeks
//     the most recent definition of the name on the executed path; none
//     (optimized away, or not on this path) prints `<dangling: x>`.
//   - strong: the print has the values as SSA operands. They stay alive
//     through every pass, and every pass treats the print as a user.
// sea-print-strengthen / sea-print-weaken convert between the forms.
//===----------------------------------------------------------------------===//

def Sea_PrintResource : Resource<"::sea::PrintResource">;

def Sea_PrintOp : Sea_Op<"print", [MemoryEffects<[MemWrite<Sea_PrintResource>]>]> {
  let summary = "Show a value in a counterexample";
  let description = [{
    ```mlir
    sea.print "reached here"                  // a tag line
    sea.print "amount" = "x" : i64            // weak: the value named x
    sea.print "delta" = "nd.3" : i64 {fmt = "s"}
    sea.print "amount" %x : i64               // strong: an operand
    sea.print "p" = "p" : !sea.ptr<64>        // &heap.3[8]
    sea.print "pt" = "s" : !sea.struct<i32, i32> {names = ["x", "y"]}
    sea.print "pt" %x, %y : !sea.struct<i32, i32>   // strong, by leaves
    ```
    `type` is the type of the value shown. Weak: a definition of another
    type prints `<dangling: x>` too (a pointer and an address of the same
    width agree: placement turns one into the other). Strong: the operand
    is a value of that type, or, for a struct, its leaves in order
    (sea-scalarize-structs makes that form). `fmt`: auto (default:
    unsigned decimal below 2^32, else hex, plus the signed value when it is
    negative and small), u, s, x, b, c, bits. Pointers and addresses always
    print as pointers (`&heap.3[8]`); a struct prints one line per leaf,
    tagged by `names` (else 0, 1, ...).
  }];
  let arguments = (ins Variadic<Sea_RegValue>:$args, StrAttr:$tag,
                   OptionalAttr<StrAttr>:$ref, OptionalAttr<TypeAttr>:$type,
                   OptionalAttr<StrAttr>:$fmt,
                   OptionalAttr<StrArrayAttr>:$names);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    /// The type of the value shown (null for a tag line).
    ::mlir::Type getShownType() {
      if (getType())
        return *getType();
      return getArgs().empty() ? ::mlir::Type() : getArgs()[0].getType();
    }
    bool isStrong() { return !getArgs().empty(); }
  }];
}

def Sea_PrintMemOp : Sea_Op<"print.mem", [MemoryEffects<[MemWrite<Sea_PrintResource>]>]> {
  let summary = "Show memory contents in a counterexample";
  let description = [{
    ```mlir
    sea.print.mem "bal" = "nd.2"["p"] : i64 {fmt = "s"}          // weak
    sea.print.mem "v" = "m"["p"] : i32 {count = 4, offset = 8}
    sea.print.mem "buf" = "nd.2"["p"] : i8 {fmt = "hexdump", count_ref = "n"}
    sea.print.mem "bal" %m[%p] : !sea.mem<!sea.ptr<64>, i8> -> i64   // strong
    sea.print.mem "buf" %m[%p], %n : !sea.mem<!sea.ptr<64>, i8>, i64 -> i8
        {fmt = "hexdump"}
    ```
    Reads `count` elements (default 1; or as many as the value of the
    count name / operand) of `type` (an integer of whole bytes,
    little-endian, or a pointer / address) from `offset` bytes past the
    address, in the memory. Weak: the memory is named by `nd.<site>` or by
    the NameLoc name of any of its versions (its region); strong: the
    operand is the memory version current at the print. It shows the
    region's contents at the print's point; nothing is read through the
    program's nondet source that the program does not read itself (an
    unread cell is peeked: `..` marks a value the model leaves
    unconstrained, `??` one no source gives). `hexdump` (i8 only) prints
    SeaHorn's HexDump layout. A struct in memory is a sea.print.scope plus
    one print.mem per field.
  }];
  let arguments = (ins Variadic<AnyType>:$args, StrAttr:$tag,
                   OptionalAttr<StrAttr>:$mem, OptionalAttr<StrAttr>:$addr,
                   TypeAttr:$type, OptionalAttr<StrAttr>:$fmt,
                   OptionalAttr<ConfinedAttr<I64Attr, [IntPositive]>>:$count,
                   OptionalAttr<StrAttr>:$count_ref,
                   OptionalAttr<I64Attr>:$offset);
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
  let extraClassDeclaration = [{
    bool isStrong() { return !getArgs().empty(); }
    /// Strong form: the memory, the address, the count (or null).
    ::mlir::Value getMemValue() { return isStrong() ? getArgs()[0] : nullptr; }
    ::mlir::Value getAddrValue() { return isStrong() ? getArgs()[1] : nullptr; }
    ::mlir::Value getCountValue() {
      return getArgs().size() > 2 ? getArgs()[2] : nullptr;
    }
  }];
}

def Sea_PrintScopeOp : Sea_Op<"print.scope", [MemoryEffects<[MemWrite<Sea_PrintResource>]>]> {
  let summary = "Open a print scope: later tags print as `<tag>.<name>`";
  let description = [{
    Matched by tag at run time with sea.print.end (cvlr's scope_start /
    scope_end). An unmatched end prints a note; it never fails a run.
  }];
  let arguments = (ins StrAttr:$tag);
  let assemblyFormat = "$tag attr-dict";
  let hasVerifier = 1;
}

def Sea_PrintEndOp : Sea_Op<"print.end", [MemoryEffects<[MemWrite<Sea_PrintResource>]>]> {
  let summary = "Close the print scope opened with the same tag";
  let arguments = (ins StrAttr:$tag);
  let assemblyFormat = "$tag attr-dict";
  let hasVerifier = 1;
}

#endif // SEA_OPS
