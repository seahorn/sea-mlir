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

// A memory that starts as a constant gives a fresh object the constant's
// contents: its memhavoc is not a no-op (dropping it made the VC unsat
// while the program fails; test/VC/drop-fresh-memhavoc-const.mlir).
// CHECK-LABEL: sea.program @const_root
// CHECK: sea.memhavoc
sea.program @const_root {
  %z = sea.const 9 : i8
  %m0 = sea.mem.const %z : !m
  %n = sea.const 4 : i64
  %a = sea.malloc %n align 4 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.memhavoc %m0[%a], %n [2] inplace : !m
  %x = sea.load %m1[%a] : !m -> i8
  sea.halt
}

// The same behind updates that precede the allocation.
// CHECK-LABEL: sea.program @const_root_behind
// CHECK: sea.memhavoc
sea.program @const_root_behind {
  %z = sea.const 9 : i8
  %m0 = sea.mem.const %z : !m
  %n = sea.const 4 : i64
  %b = sea.malloc %n align 4 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%b], %z inplace : !m, i8
  %a = sea.malloc %n align 4 [2] : i64 -> !sea.ptr<64>
  %m2 = sea.memhavoc %m1[%a], %n [3] inplace : !m
  %x = sea.load %m2[%a] : !m -> i8
  sea.halt
}

// A nondet memory behind updates that precede the allocation: fresh.
// CHECK-LABEL: sea.program @nondet_root_behind
// CHECK-NOT: sea.memhavoc
sea.program @nondet_root_behind {
  %m0 = sea.mem.nondet "m" [0] : !m
  %z = sea.const 9 : i8
  %n = sea.const 4 : i64
  %b = sea.malloc %n align 4 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%b], %z inplace : !m, i8
  %a = sea.malloc %n align 4 [2] : i64 -> !sea.ptr<64>
  %m2 = sea.memhavoc %m1[%a], %n [3] inplace : !m
  %x = sea.load %m2[%a] : !m -> i8
  sea.halt
}

// An update in a join after the allocation (one branch), not dominated by
// it, can follow it: through the pointer phi (the object or null) the
// store writes the object, so the memhavoc stays
// (test/VC/drop-fresh-memhavoc-join.mlir).
// CHECK-LABEL: sea.program @join_store
// CHECK: sea.memhavoc
sea.program @join_store {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c = sea.nondet "c" [3] : i1
  %c4 = sea.const 4 : i64
  %v = sea.const 9 : i8
  sea.cond_br %c, ^l, ^r
^l:
  %a = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  sea.br ^j(%a : !sea.ptr<64>)
^r:
  %n = sea.ptr.null : !sea.ptr<64>
  sea.br ^j(%n : !sea.ptr<64>)
^j(%p: !sea.ptr<64>):
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %m2 = sea.memhavoc %m1[%p], %c4 [2] inplace : !m
  %x = sea.load %m2[%p] : !m -> i8
  sea.halt
}

// The same join, its store to another object: still a no-op.
// CHECK-LABEL: sea.program @join_store_other
// CHECK-NOT: sea.memhavoc
sea.program @join_store_other {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c = sea.nondet "c" [3] : i1
  %c4 = sea.const 4 : i64
  %v = sea.const 9 : i8
  %b = sea.malloc %c4 align 4 [4] : i64 -> !sea.ptr<64>
  sea.cond_br %c, ^l, ^r
^l:
  %a = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  sea.br ^j(%a : !sea.ptr<64>)
^r:
  %n = sea.ptr.null : !sea.ptr<64>
  sea.br ^j(%n : !sea.ptr<64>)
^j(%p: !sea.ptr<64>):
  %m1 = sea.store %m0[%b], %v inplace : !m, i8
  %m2 = sea.memhavoc %m1[%p], %c4 [2] inplace : !m
  %x = sea.load %m2[%p] : !m -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
