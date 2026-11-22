// RUN: sea-opt %s -split-input-file -verify-diagnostics

// seaimp.var init (a constant memory start) and seaimp.mem.fill.

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @init_and_site {
  %z = sea.const 0 : i8
  // expected-error@+1 {{a variable with init has no site}}
  %v = seaimp.var "v" [0] init %z : !seaimp.var<!m>
  sea.halt
}

// -----

sea.program @no_site {
  // expected-error@+1 {{needs a site (its nondet initial value), or init for a memory}}
  %x = seaimp.var "x" : !seaimp.var<i32>
  sea.halt
}

// -----

sea.program @shadow_without_init {
  // expected-error@+1 {{shadow is a flag of init}}
  %v = seaimp.var "v" [0] shadow : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  sea.halt
}

// -----

sea.program @init_not_a_cell {
  %z = sea.const 0 : i16
  // expected-error@+1 {{use of value '%z' expects different type than prior uses: 'i8' vs 'i16'}}
  %v = seaimp.var "v" init %z : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // expected-note@-3 {{prior use here}}
  sea.halt
}

// -----

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @fill_cell_type {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %g = seaimp.global %mem @tab size 4 align 4 : !seaimp.var<!m> -> !sea.ptr<64>
  // expected-error@+1 {{data cells 'i32' are not the memory's cells 'i8'}}
  seaimp.mem.fill %mem[%g], dense<[1]> : tensor<1xi32> : !seaimp.var<!m>
  sea.halt
}

// -----

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @fill_empty {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %g = seaimp.global %mem @tab size 4 align 4 : !seaimp.var<!m> -> !sea.ptr<64>
  // expected-error@+1 {{data must have at least one cell}}
  seaimp.mem.fill %mem[%g], dense<> : tensor<0xi8> : !seaimp.var<!m>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
