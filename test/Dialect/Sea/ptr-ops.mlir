// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

!pm = !sea.mem<!sea.ptr<64>, i8>

// CHECK-LABEL: sea.program @ptrs
sea.program @ptrs {
  %m0 = sea.mem.nondet "heap" [0] : !pm
  %n = sea.const 16 : i64
  // CHECK: %[[P:.*]] = sea.alloca %{{.*}} align 8 [1] : i64 -> !sea.ptr<64>
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  // CHECK: sea.malloc %{{.*}} align 16 [2] : i64 -> !sea.ptr<64>
  %h = sea.malloc %n align 16 [2] : i64 -> !sea.ptr<64>
  // CHECK: sea.global @counter size 4 align 4 : !sea.ptr<64>
  %g = sea.global @counter size 4 align 4 : !sea.ptr<64>
  // CHECK: sea.func_addr @main : !sea.ptr<64>
  %f = sea.func_addr @main : !sea.ptr<64>
  %one = sea.const 1 : i64
  // CHECK: sea.ptr.add %[[P]], %{{.*}} : !sea.ptr<64>, i64
  %q = sea.ptr.add %p, %one : !sea.ptr<64>, i64
  // CHECK: sea.ptr.diff
  %d = sea.ptr.diff %q, %p : !sea.ptr<64> -> i64
  // CHECK: sea.ptr.cmp eq
  %c = sea.ptr.cmp eq, %p, %q : !sea.ptr<64>
  // CHECK: sea.ptr.null : !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  // CHECK: sea.ptr.to_int
  %i = sea.ptr.to_int %p : !sea.ptr<64> to i64
  // CHECK: sea.ptr.from_int
  %r = sea.ptr.from_int %i : i64 to !sea.ptr<64>
  %v = sea.const 7 : i8
  // CHECK: sea.store {{.*}} inplace : !sea.mem<!sea.ptr<64>, i8>, i8
  %m1 = sea.store %m0[%p], %v inplace : !pm, i8
  %x = sea.load %m1[%q] : !pm -> i8
  // CHECK: sea.free %{{.*}} : !sea.ptr<64>
  sea.free %h : !sea.ptr<64>
  sea.halt
}

// CHECK-LABEL: sea.program @placed
sea.program @placed {
  // CHECK: %{{.*}} = sea.segment "alloca.1" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %b = sea.segment "alloca.1" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
