// RUN: sea-opt %s -split-input-file -verify-diagnostics

// A load or store of a keyed memory is one whole cell: keyed cells are
// independent, so a value narrower or wider than the cell is rejected.

!k = !sea.mem<!sea.addr<64>, i32, keyed>
sea.program @load_wider {
  %m = sea.mem.nondet "m" [0] : !k
  %z = sea.const 0 : i64
  %a = sea.addr.from_int %z : i64 to !sea.addr<64>
  // expected-error@+1 {{result type 'i64' is not one cell 'i32': an access of a keyed memory is one whole cell}}
  %x = sea.load %m[%a] : !k -> i64
  sea.halt
}

// -----

!k = !sea.mem<!sea.addr<64>, i32, keyed>
sea.program @load_narrower {
  %m = sea.mem.nondet "m" [0] : !k
  %z = sea.const 0 : i64
  %a = sea.addr.from_int %z : i64 to !sea.addr<64>
  // expected-error@+1 {{result type 'i8' is not one cell 'i32': an access of a keyed memory is one whole cell}}
  %x = sea.load %m[%a] : !k -> i8
  sea.halt
}

// -----

!k = !sea.mem<!sea.ptr<64>, i8, keyed>
sea.program @store_wider {
  %m = sea.mem.nondet "m" [0] : !k
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %v = sea.const 0 : i32
  // expected-error@+1 {{value type 'i32' is not one cell 'i8': an access of a keyed memory is one whole cell}}
  %m1 = sea.store %m[%p], %v inplace : !k, i32
  sea.halt
}

// -----

!k = !sea.mem<!sea.ptr<64>, i64, keyed>
sea.program @store_narrower {
  %m = sea.mem.nondet "m" [0] : !k
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %v = sea.const 0 : i16
  // expected-error@+1 {{value type 'i16' is not one cell 'i64': an access of a keyed memory is one whole cell}}
  %m1 = sea.store %m[%p], %v inplace : !k, i16
  sea.halt
}

// -----

// A pointer value is a cell of its width too.
!k = !sea.mem<!sea.ptr<32>, i32, keyed>
sea.program @store_pointer_wider {
  %m = sea.mem.nondet "m" [0] : !k
  %n = sea.const 8 : i32
  %p = sea.alloca %n align 8 [1] : i32 -> !sea.ptr<32>
  %c = sea.const 8 : i64
  %q = sea.alloca %c align 8 [2] : i64 -> !sea.ptr<64>
  // expected-error@+1 {{value type '!sea.ptr<64>' is not one cell 'i32': an access of a keyed memory is one whole cell}}
  %m1 = sea.store %m[%p], %q inplace : !k, !sea.ptr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
