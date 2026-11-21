// RUN: sea-opt %s -split-input-file -verify-diagnostics -allow-unregistered-dialect

// The verifier admits only variables sea-lower-seaimp (Mem2Reg) promotes.

sea.program @not_entry {
  %c = sea.const 1 : i1
  sea.br ^bb1
^bb1:
  // expected-error@+1 {{must be in the entry block of its sea.program}}
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  sea.halt
}

// -----

sea.program @dup_site {
  // expected-note@+1 {{previous use here}}
  %a = sea.nondet "nd" [1] : i32
  // expected-error@+1 {{duplicate nondet site 1}}
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  sea.halt
}

// -----

sea.program @dup_site_vars {
  // expected-note@+1 {{previous use here}}
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  // expected-error@+1 {{duplicate nondet site 1}}
  %y = seaimp.var "y" [1] : !seaimp.var<i8>
  sea.halt
}

// -----

sea.program @escape_branch {
  // expected-error@+1 {{the variable handle may be used only as the variable operand of seaimp ops}}
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  // expected-note@+1 {{used here by sea.br}}
  sea.br ^bb1(%x : !seaimp.var<i32>)
^bb1(%y : !seaimp.var<i32>):
  sea.halt
}

// -----

sea.program @escape_other_op {
  // expected-error@+1 {{may be used only as the variable operand}}
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  // expected-note@+1 {{used here by test.use}}
  "test.use"(%x) : (!seaimp.var<i32>) -> ()
  sea.halt
}

// -----

sea.program @nested_region {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  "test.region"() ({
    // expected-error@+1 {{must be in the region of its seaimp.var}}
    %v = seaimp.get %x : !seaimp.var<i32>
    "test.yield"() : () -> ()
  }) : () -> ()
  sea.halt
}

// -----

sea.program @block_arg_handle {
  %c = sea.const 1 : i1
  sea.halt
^bb1(%v : !seaimp.var<i32>):
  // expected-error@+1 {{the variable operand must be the result of a seaimp.var}}
  %r = seaimp.get %v : !seaimp.var<i32>
  sea.halt
}

// -----

sea.program @set_type {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %a = sea.const 1 : i8
  // expected-error@+1 {{value type is the variable's value type}}
  "seaimp.set"(%x, %a) : (!seaimp.var<i32>, i8) -> ()
  sea.halt
}

// -----

sea.program @get_type {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  // expected-error@+1 {{result type is the variable's value type}}
  %v = "seaimp.get"(%x) : (!seaimp.var<i32>) -> i8
  sea.halt
}

// -----

sea.program @addr_var {
  // expected-error@+1 {{address variables are not supported}}
  %a = seaimp.var "a" [1] : !seaimp.var<!sea.addr<64>>
  sea.halt
}

// -----

sea.program @signed_var {
  // expected-error@+1 {{a variable holds a signless integer}}
  %a = seaimp.var "a" [1] : !seaimp.var<si32>
  sea.halt
}

// -----

// expected-error@+1 {{must be in a sea.program}}
%x = seaimp.var "x" [1] : !seaimp.var<i32>

// -----

sea.program @region_not_mem {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  // expected-error@+1 {{must be seaimp variable holding a memory}}
  %v = "seaimp.load"(%x, %p) : (!seaimp.var<i32>, !sea.ptr<64>) -> i32
  sea.halt
}

// -----

sea.program @region_not_mem_custom {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  // expected-error@+1 {{'var' must be seaimp variable holding a memory, but got '!seaimp.var<i32>'}}
  %v = seaimp.load %x[%p] : !seaimp.var<i32> -> i32
  sea.halt
}

// -----

sea.program @region_index {
  %m = seaimp.var "m" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<32>
  // expected-error@+1 {{index type indexes the region}}
  %v = "seaimp.load"(%m, %p) : (!seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, !sea.ptr<32>) -> i32
  sea.halt
}

// -----

sea.program @region_cells {
  %m = seaimp.var "m" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i32, aligned>>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  %v = sea.const 1 : i8
  // expected-error@+1 {{value type 'i8' does not fit cells 'i32'}}
  seaimp.store %m[%p], %v : !seaimp.var<!sea.mem<!sea.ptr<64>, i32, aligned>>, i8
  sea.halt
}

// -----

sea.program @region_align {
  %m = seaimp.var "m" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  // expected-error@+1 {{align must be a power of two, got 3}}
  %v = seaimp.load %m[%p] align 3 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i32
  sea.halt
}

// -----

sea.program @memmove_types {
  %m = seaimp.var "m" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %w = seaimp.var "w" [2] : !seaimp.var<!sea.mem<!sea.ptr<64>, i32, aligned>>
  %p = sea.ptr.nondet "p" [3] : !sea.ptr<64>
  %n = sea.const 4 : i64
  // expected-error@+1 {{all of {var, src} have same type}}
  "seaimp.memmove"(%m, %p, %w, %p, %n) : (!seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, !sea.ptr<64>, !seaimp.var<!sea.mem<!sea.ptr<64>, i32, aligned>>, !sea.ptr<64>, i64) -> ()
  sea.halt
}

// -----

sea.program @memhavoc_site {
  // expected-note@+1 {{previous use here}}
  %m = seaimp.var "m" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  %n = sea.const 4 : i64
  // expected-error@+1 {{duplicate nondet site 1}}
  seaimp.memhavoc %m[%p], %n [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  sea.halt
}

// -----

sea.program @region_nested {
  %m = seaimp.var "m" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  "test.region"() ({
    // expected-error@+1 {{must be in the region of its seaimp.var}}
    %v = seaimp.load %m[%p] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i8
    "test.yield"() : () -> ()
  }) : () -> ()
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
