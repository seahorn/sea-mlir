// RUN: sea-opt %s -split-input-file -verify-diagnostics

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @cell_type {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 4 align 4 : !sea.ptr<64>
  // expected-error@+1 {{data cells 'i16' are not the memory's cells 'i8'}}
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2]> : tensor<2xi16> inplace : !m
  sea.halt
}

// -----

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @empty {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 4 align 4 : !sea.ptr<64>
  // expected-error@+1 {{data must have at least one cell}}
  %m1 = sea.mem.fill %m0[%g], dense<> : tensor<0xi8> inplace : !m
  sea.halt
}

// -----

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @rank {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 4 align 4 : !sea.ptr<64>
  // expected-error@+1 {{data must be a 1-D list of cells}}
  %m1 = sea.mem.fill %m0[%g], dense<[[1, 2], [3, 4]]> : tensor<2x2xi8> inplace : !m
  sea.halt
}

// -----

!m = !sea.mem<!sea.addr<8>, i8>

sea.program @too_long {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  // expected-error@+1 {{data of 256 cells does not fit the 8-bit index space}}
  %m1 = sea.mem.fill %m0[%a], dense<0> : tensor<256xi8> inplace : !m
  sea.halt
}

// -----

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @index_type {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %z = sea.const 0 : i64
  %a = sea.addr.from_int %z : i64 to !sea.addr<64>
  // expected-error@+1 {{use of value '%a' expects different type than prior uses}}
  %m1 = sea.mem.fill %m0[%a], dense<[1]> : tensor<1xi8> inplace : !m
  // expected-note@-3 {{prior use here}}
  sea.halt
}

// -----

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @float_data {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 4 align 4 : !sea.ptr<64>
  // expected-error@+1 {{invalid kind of attribute specified}}
  %m1 = sea.mem.fill %m0[%g], dense<[1.0]> : tensor<1xf32> inplace : !m
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
