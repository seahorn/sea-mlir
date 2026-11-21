// RUN: not sea-run %s 2>&1 | FileCheck %s

// Struct block arguments swapped on a back edge are bound as a parallel copy:
// after one swap s0 holds B (16 bytes), so s0's pointer + 8 is in bounds and
// s1's (A, 1 byte) is not.
// CHECK: at: loc("p1_load"
// CHECK-NEXT: message: memory access out of bounds: offset 8 in heap.1 of size 1
!pm = !sea.mem<!sea.ptr<64>, i8>
!st = !sea.struct<!sea.ptr<64>, i8>
sea.program @structswap {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !pm
  %c1 = sea.const 1 : i64
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c1 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %sa = sea.struct.create %A, %z : !sea.ptr<64>, i8 -> !st
  %sb = sea.struct.create %B, %z : !sea.ptr<64>, i8 -> !st
  %t = sea.const 1 : i1
  sea.br ^h(%sa, %sb, %t : !st, !st, i1)
^h(%s0 : !st, %s1 : !st, %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%s1, %s0, %f : !st, !st, i1), ^use
^use:
  %p0 = sea.struct.extract %s0[0] : !st -> !sea.ptr<64>
  %q0 = sea.ptr.add %p0, %c8 : !sea.ptr<64>, i64
  %x = sea.load %m[%q0] : !pm -> i8
  %p1 = sea.struct.extract %s1[0] : !st -> !sea.ptr<64>
  %q1 = sea.ptr.add %p1, %c8 : !sea.ptr<64>, i64
  %y = sea.load %m[%q1] : !pm -> i8 loc("p1_load")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
