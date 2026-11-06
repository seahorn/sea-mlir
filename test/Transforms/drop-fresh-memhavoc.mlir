// RUN: sea-opt %s --sea-drop-fresh-memhavoc | FileCheck %s

// memhavoc of an object nothing wrote since its allocation is a no-op (its
// contents are unconstrained already); elsewhere it stays.
!m = !sea.mem<!sea.ptr<64>, i8>

// CHECK-LABEL: sea.program @fresh
// CHECK-NOT: sea.memhavoc
sea.program @fresh {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %v = sea.const 1 : i8
  %other = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%other], %v inplace : !m, i8
  %a = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  // a write to another object after the allocation does not matter
  %m2 = sea.store %m1[%other], %v inplace : !m, i8
  %m3 = sea.memhavoc %m2[%a], %n [3] inplace : !m
  %x = sea.load %m3[%a] : !m -> i8
  sea.halt
}

// CHECK-LABEL: sea.program @written
// CHECK: sea.memhavoc
sea.program @written {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %v = sea.const 1 : i8
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %four = sea.const 4 : i64
  %q = sea.ptr.add %a, %four : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %m2 = sea.memhavoc %m1[%a], %n [2] inplace : !m
  sea.halt
}

// Written on one branch only: not fresh at the join.
// CHECK-LABEL: sea.program @join
// CHECK: sea.memhavoc
sea.program @join {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %v = sea.const 1 : i8
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [2] : i1
  sea.cond_br %c, ^w, ^j(%m0 : !m)
^w:
  %m1 = sea.store %m0[%a], %v inplace : !m, i8
  sea.br ^j(%m1 : !m)
^j(%m2: !m):
  %m3 = sea.memhavoc %m2[%a], %n [3] inplace : !m
  sea.halt
}

// The allocator pattern: the pointer is a fresh allocation or null (failed
// allocation) at a join; a havoc of null touches nothing.
// CHECK-LABEL: sea.program @alloc_or_null
// CHECK-NOT: sea.memhavoc
sea.program @alloc_or_null {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.nondet "n" [1] : i64
  %fail = sea.nondet "fail" [2] : i1
  %null = sea.ptr.null : !sea.ptr<64>
  sea.cond_br %fail, ^j(%m0, %null : !m, !sea.ptr<64>), ^a
^a:
  %p = sea.alloca %n align 16 [3] : i64 -> !sea.ptr<64>
  sea.br ^j(%m0, %p : !m, !sea.ptr<64>)
^j(%m1: !m, %q: !sea.ptr<64>):
  %m2 = sea.memhavoc %m1[%q], %n [4] inplace : !m
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
