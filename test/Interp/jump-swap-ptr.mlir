// RUN: not sea-run %s | FileCheck %s

// A back edge that passes the block's own pointer arguments swapped binds
// them as a parallel copy: after one swap p0 is B (16 bytes), so p0+8 is in
// bounds and p1+8 (A, 1 byte) is not.
// CHECK: memory access out of bounds: offset 8 in heap.1 of size 1
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @ptrswap {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !pm
  %c1 = sea.const 1 : i64
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c1 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %t = sea.const 1 : i1
  sea.br ^h(%A, %B, %t : !sea.ptr<64>, !sea.ptr<64>, i1)
^h(%p0 : !sea.ptr<64>, %p1 : !sea.ptr<64>, %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%p1, %p0, %f : !sea.ptr<64>, !sea.ptr<64>, i1), ^use
^use:
  %q0 = sea.ptr.add %p0, %c8 : !sea.ptr<64>, i64
  %x = sea.load %m[%q0] : !pm -> i8
  %q1 = sea.ptr.add %p1, %c8 : !sea.ptr<64>, i64
  %y = sea.load %m[%q1] : !pm -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
