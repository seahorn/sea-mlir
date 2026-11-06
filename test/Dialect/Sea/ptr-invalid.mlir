// RUN: sea-opt %s -split-input-file -verify-diagnostics

sea.program @size_width {
  %n = sea.const 16 : i32
  // expected-error@+1 {{size must have the pointer width}}
  %p = sea.alloca %n align 8 [0] : i32 -> !sea.ptr<64>
  sea.halt
}

// -----

sea.program @align {
  %n = sea.const 16 : i64
  // expected-error@+1 {{alignment must be a power of 2}}
  %p = sea.alloca %n align 6 [0] : i64 -> !sea.ptr<64>
  sea.halt
}

// -----

sea.program @dup_alloc_site {
  %n = sea.const 1 : i64
  // expected-note@+1 {{previous use here}}
  %p = sea.alloca %n align 1 [0] : i64 -> !sea.ptr<64>
  // expected-error@+1 {{duplicate nondet site 0}}
  %q = sea.malloc %n align 1 [0] : i64 -> !sea.ptr<64>
  sea.halt
}

// -----

sea.program @global_mismatch {
  // expected-note@+1 {{other use here}}
  %a = sea.global @g size 4 align 4 : !sea.ptr<64>
  // expected-error@+1 {{size or alignment differs from another use of @g}}
  %b = sea.global @g size 8 align 4 : !sea.ptr<64>
  sea.halt
}

// -----

sea.program @ptr_index {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %z = sea.const 0 : i64
  %a = sea.addr.from_int %z : i64 to !sea.addr<64>
  // expected-error@+1 {{use of value '%a' expects different type than prior uses}}
  %x = sea.load %m[%a] : !sea.mem<!sea.ptr<64>, i8> -> i8
  // expected-note@-3 {{prior use here}}
  sea.halt
}

// -----

sea.program @from_bits_width {
  %z = sea.const 0 : i32
  // expected-error@+1 {{value must have the address width}}
  %a = sea.addr.from_bits %z : i32 to !sea.addr<64>
  sea.halt
}

// -----

sea.program @to_bits_width {
  %z = sea.const 0 : i64
  %a = sea.addr.from_bits %z : i64 to !sea.addr<64>
  // expected-error@+1 {{result must have the address width}}
  %b = sea.addr.to_bits %a : !sea.addr<64> to i32
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
