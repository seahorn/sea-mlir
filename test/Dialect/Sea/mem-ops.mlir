// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// CHECK: sea.mem.nondet "heap" [0] : !sea.mem<!sea.addr<64>, i8>
// CHECK: sea.addr.add {{.*}} : !sea.addr<64>, i64
// CHECK: sea.store {{.*}} inplace : !sea.mem<!sea.addr<64>, i8>, i8
// CHECK: sea.memset
// CHECK: sea.memmove
// CHECK: sea.memhavoc {{.*}} [2] inplace
// CHECK: sea.mem.nondet "words" [3] : !sea.mem<!sea.addr<61, unit = 64>, i64>
// CHECK: sea.load {{.*}} -> !sea.addr<64>
sea.program @mem {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.addr<64>, i8>
  %i = sea.nondet "nondet_u64" [1] : i64
  %p = sea.addr.from_int %i : i64 to !sea.addr<64>
  %one = sea.const 1 : i64
  %q = sea.addr.add %p, %one : !sea.addr<64>, i64
  %d = sea.addr.diff %q, %p : !sea.addr<64> -> i64
  %c = sea.addr.cmp ult, %p, %q : !sea.addr<64>
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !sea.mem<!sea.addr<64>, i8>, i8
  %n = sea.const 4 : i64
  %m2 = sea.memset %m1[%q], %v, %n inplace : !sea.mem<!sea.addr<64>, i8>
  %m3 = sea.memmove %m2[%p], %m2[%q], %n inplace : !sea.mem<!sea.addr<64>, i8>
  %m4 = sea.memhavoc %m3[%p], %n [2] inplace : !sea.mem<!sea.addr<64>, i8>
  %x = sea.load %m4[%q] : !sea.mem<!sea.addr<64>, i8> -> i8
  %w = sea.mem.nondet "words" [3] : !sea.mem<!sea.addr<61, unit = 64>, i64>
  %wz = sea.const 0 : i61
  %wa = sea.addr.from_int %wz : i61 to !sea.addr<61, unit = 64>
  %pa = sea.load %w[%wa] : !sea.mem<!sea.addr<61, unit = 64>, i64> -> !sea.addr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
