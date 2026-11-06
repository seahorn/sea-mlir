// RUN: sea-opt %s -split-input-file -verify-diagnostics

// expected-error@+1 {{entry block must have no arguments}}
sea.program @entry_args {
^bb0(%x : i32):
  sea.halt
}

// -----

sea.program @dup_site {
  // expected-note@+1 {{previous use here}}
  %a = sea.nondet "nondet_u32" [1] : i32
  // expected-error@+1 {{duplicate nondet site 1}}
  %b = sea.nondet "nondet_u8" [1] : i8
  sea.halt
}

// -----

sea.program @neg_site {
  // expected-error@+1 {{whose value is non-negative}}
  %a = sea.nondet "nondet_u32" [-1] : i32
  sea.halt
}

// -----

sea.program @zext_narrow {
  %a = sea.const 1 : i32
  // expected-error@+1 {{result must be wider than the input}}
  %b = sea.zext %a : i32 to i32
  sea.halt
}

// -----

sea.program @sext_narrow {
  %a = sea.const 1 : i32
  // expected-error@+1 {{result must be wider than the input}}
  %b = sea.sext %a : i32 to i8
  sea.halt
}

// -----

sea.program @trunc_wide {
  %a = sea.const 1 : i32
  // expected-error@+1 {{result must be narrower than the input}}
  %b = sea.trunc %a : i32 to i64
  sea.halt
}

// -----

sea.program @mixed_widths {
  %a = sea.const 1 : i32
  // expected-note@+1 {{prior use here}}
  %b = sea.const 1 : i8
  // expected-error@+1 {{use of value '%b' expects different type than prior uses}}
  %c = sea.add %a, %b : i32
  sea.halt
}

// -----

sea.program @not_bv {
  // expected-error@+1 {{must be signless integer}}
  %a = sea.const 1.0 : f32
  sea.halt
}

// -----

sea.program @assert_wide {
  // expected-note@+1 {{prior use here}}
  %a = sea.const 1 : i32
  // expected-error@+1 {{use of value '%a' expects different type than prior uses}}
  sea.assert %a
  sea.halt
}

// -----

sea.program @no_terminator {
  %a = sea.const 1 : i32
// expected-error@-1 {{block with no terminator}}
}

// -----

// expected-error@+1 {{'sea.halt' op expects parent op 'sea.program'}}
sea.halt

// -----

sea.program @bswap_width {
  %a = sea.const 1 : i24
  // expected-error@+1 {{width must be a multiple of 16}}
  %b = sea.bswap %a : i24
  sea.halt
}

// -----

sea.program @struct_field_type {
  %a = sea.const 1 : i16
  // expected-error@+1 {{operand types must be the struct's field types}}
  %s = sea.struct.create %a : i16 -> !sea.struct<i32>
  sea.halt
}

// -----

sea.program @struct_index {
  %a = sea.const 1 : i16
  %s = sea.struct.create %a : i16 -> !sea.struct<i16>
  // expected-error@+1 {{field index out of range}}
  %x = sea.struct.extract %s[1] : !sea.struct<i16> -> i16
  sea.halt
}


// -----

// Interim layout guard: pointer-indexed memory has byte cells.
sea.program @ptr_word_cells {
  // expected-error@+1 {{pointer-indexed memory has byte cells (i8), got 'i64'}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i64>
  sea.halt
}

// -----

// Interim layout guard: an address-indexed cell is one address unit.
sea.program @addr_unit_mismatch {
  // expected-error@+1 {{memory cells must be one address unit (8 bits), got 'i32'}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i32>
  sea.halt
}

// -----

sea.program @align_pow2 {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  // expected-error@+1 {{align must be a power of two, got 3}}
  %x = sea.load %m[%p] align 3 : !sea.mem<!sea.ptr<64>, i8> -> i32
  sea.halt
}

// -----

sea.program @word_of_bytes {
  // expected-error@+1 {{a word layout has cells of k > 1 bytes, got 'i8'}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8, aligned>
  sea.halt
}

// -----

sea.program @word_on_word_addresses {
  // expected-error@+1 {{word layouts need byte addresses (unit = 8)}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<61, unit = 64>, i64, aligned>
  sea.halt
}

// -----

sea.program @deref_len_width {
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [0] : i64 -> !sea.ptr<64>
  %l = sea.const 1 : i32
  // expected-error@+1 {{length must have the pointer width}}
  %d = sea.ptr.dereferenceable %p, %l : !sea.ptr<64>, i32
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
