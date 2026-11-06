// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaTypes.td - sea dialect types --------------------*- tablegen -*-===//

#ifndef SEA_TYPES
#define SEA_TYPES

include "sea/Dialect/Sea/SeaDialect.td"
include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"

class Sea_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<Sea_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def Sea_AddrType : Sea_Type<"Addr", "addr"> {
  let summary = "An address in a flat address space";
  let description = [{
    A `width`-bit address. One address unit covers `unit` bits (default 8:
    byte addressing), so adding 1 moves one unit. Semantically a bit-vector;
    only address operations accept it (no arithmetic on addresses).

    ```mlir
    !sea.addr<64>            // byte-addressed, 64-bit
    !sea.addr<61, unit = 64> // word-addressed
    ```
  }];
  let parameters = (ins "unsigned":$width,
                        DefaultValuedParameter<"unsigned", "8">:$unit);
  let assemblyFormat = "`<` $width (`,` `unit` `=` $unit^)? `>`";
  let genVerifyDecl = 1;
}

def Sea_PtrType : Sea_Type<"Ptr", "ptr"> {
  let summary = "An abstract pointer: (object, offset)";
  let description = [{
    A pointer into an object; `width` is the width of offsets and sizes
    (the program's pointer size). Pointers have no address until placement
    (docs/design/Memory-Step2.md) lowers them to `!sea.addr`.

    ```mlir
    !sea.ptr<64>
    ```
  }];
  let parameters = (ins "unsigned":$width);
  let assemblyFormat = "`<` $width `>`";
  let genVerifyDecl = 1;
}

def Sea_MemLayout : I32EnumAttr<"MemLayout", "memory cell layout", [
    I32EnumAttrCase<"exact", 0>,
    I32EnumAttrCase<"aligned", 1>,
    I32EnumAttrCase<"unaligned", 2>,
    I32EnumAttrCase<"keyed", 3>,
  ]> {
  let cppNamespace = "::sea";
  let genSpecializedAttr = 0;
}

def Sea_MemLayoutParam : EnumParameter<Sea_MemLayout> {
  let defaultValue = "::sea::MemLayout::exact";
}

def Sea_MemType : Sea_Type<"Mem", "mem"> {
  let summary = "A memory version: a map from addresses to cells";
  let description = [{
    A memory maps indices to cells of type `cell` (a signless integer). The
    index is an address (`!sea.addr`, concrete level) or a pointer
    (`!sea.ptr`, abstract level). A cell is the unit of load and store.
    Memory is a value: ops that change it return a new version (Mem-SSA,
    one chain per region).

    The layout says where cells are. Indices, offsets and lengths are
    always in index units (bytes, for byte-addressed indices).
    - exact (default): one cell per index unit (byte memory; word-addressed
      memory of unit-sized words).
    - aligned: k-byte cells (k > 1, `cell` = 8k bits) at byte offsets that
      are multiples of k; accesses and range-op lengths are whole cells.
    - unaligned: k-byte cells at any byte offset (ignore-alignment); sound
      only if every access to a cell matches the one that stored it.
    - keyed: a cell of any width at every index unit, keyed by its address.
      Cells are independent: a cell does not cover the bytes after its
      address, so a store at a + 1 leaves the cell at a as it was. An
      access is one whole cell, and a range op over [d, d + n) acts on the
      n cells at d .. d + n - 1. memset's value (one unit) is repeated
      across the cell. It is more storage than the bytes, which is what
      metadata memories want: the offset / size companions of fat pointers
      (sea-lower-fat-pointers), never split or widened.

    ```mlir
    !sea.mem<!sea.ptr<64>, i8>             // abstract byte memory
    !sea.mem<!sea.addr<64>, i8>            // byte memory
    !sea.mem<!sea.addr<64>, i64, aligned>  // 8-byte words, aligned
    !sea.mem<!sea.ptr<64>, i64, unaligned> // 8-byte words, any offset
    !sea.mem<!sea.addr<64>, i64, keyed>    // a 64-bit cell at every byte
    !sea.mem<!sea.addr<61, unit = 64>, i64> // word-addressed
    ```
  }];
  let parameters = (ins "::mlir::Type":$index, "::mlir::IntegerType":$cell,
                        Sea_MemLayoutParam:$layout);
  let builders = [
    TypeBuilder<(ins "::mlir::Type":$index, "::mlir::IntegerType":$cell), [{
      return $_get($_ctxt, index, cell, ::sea::MemLayout::exact);
    }]>
  ];
  let skipDefaultBuilders = 0;
  let assemblyFormat =
      "`<` qualified($index) `,` $cell (`,` $layout^)? `>`";
  let genVerifyDecl = 1;
  let extraClassDeclaration = [{
    /// Width of the index (address or pointer offset): also the width of
    /// lengths and offsets used with this memory.
    unsigned getIndexWidth() const;
    /// Index units per cell: 1 for the exact layout, k for k-byte words.
    unsigned getCellUnits() const;
    /// aligned / unaligned: k-byte cells that cover k bytes
    bool isWordLayout() const {
      return getLayout() == ::sea::MemLayout::aligned ||
             getLayout() == ::sea::MemLayout::unaligned;
    }
    /// keyed: a cell at every unit, independent
    bool isKeyed() const { return getLayout() == ::sea::MemLayout::keyed; }
    /// How many copies of a unit (memset's value) make a cell.
    unsigned getUnitCopies() const;
    /// One index unit as an integer (memset's value): the cell for the
    /// exact layout, i8 for word layouts.
    ::mlir::IntegerType getUnitType() const;
  }];
}

def Sea_StructType : Sea_Type<"Struct", "struct"> {
  let summary = "A register aggregate: a tuple of values";
  let description = [{
    LLVM's first-class aggregates (structs and arrays as values: insertvalue,
    extractvalue, aggregate phis and selects). Fields are bit-vectors,
    addresses, pointers or structs; at least one. A struct has no memory
    layout: it lives in registers only (`sea-scalarize-structs` replaces it
    by its fields).
    ```mlir
    !sea.struct<i16, i32>
    !sea.struct<!sea.ptr<64>, !sea.struct<i8, i8>>
    ```
  }];
  let parameters = (ins ArrayRefParameter<"::mlir::Type">:$fields);
  let assemblyFormat = "`<` $fields `>`";
  let genVerifyDecl = 1;
  let extraClassDeclaration = [{
    /// Number of non-struct leaves (fields of nested structs counted).
    unsigned getNumLeaves() const;
    /// Index of the first leaf of field `i`.
    unsigned getLeafOffset(unsigned i) const;
  }];
}

#endif // SEA_TYPES
