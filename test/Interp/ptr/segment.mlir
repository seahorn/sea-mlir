// RUN: sea-run %s --program=base --trace --missing=zero | FileCheck %s

// A sea.segment evaluates to its base; accesses through it and through
// address arithmetic from it reach the same cells.
// CHECK: value: sea.segment {{.*}} : !sea.addr<64> = 3221225456
// CHECK: value: sea.addr.add {{.*}} : !sea.addr<64> = 3221225460
// CHECK: value: sea.load {{.*}} = 7
// CHECK: status: halted
sea.program @base {
  %b = sea.segment "stack.3" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %four = sea.const 4 : i64
  %q = sea.addr.add %b, %four : !sea.addr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !sea.mem<!sea.addr<64>, i8>, i8
  %x = sea.load %m1[%q] : !sea.mem<!sea.addr<64>, i8> -> i8
  %c = sea.icmp eq, %x, %v : i8
  sea.assume %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
