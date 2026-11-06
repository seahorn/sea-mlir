// Every object has its own base (sea-place-static; docs/design/SeaM-null.md,
// "Shadow memories"): p = malloc(0) keeps size 0 but takes one unit of
// address space, so p == q (q = malloc(4)) is false in every layout, as in
// the abstract interpreter (objects, not addresses): the read goes through
// r, which holds 0: unsat, plain and --seam. The segment of p keeps size 0.
// RUN: sea-vc %s --lower | FileCheck %s
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefixes=CHECK,SEG
// RUN: sea-run %s | FileCheck %s --check-prefix=INTERP
// SEG: object: name=obj.heap.1 order=0 kind=heap base=134512640 size=0
// SEG: object: name=obj.heap.2 order=1 kind=heap base=134512644 size=4
// CHECK: result: unsat
// INTERP: status: halted
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @zero {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c0 = sea.const 0 : i64
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c0 align 4 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c4 align 4 [2] : i64 -> !sea.ptr<64>
  %r = sea.malloc %c4 align 4 [3] : i64 -> !sea.ptr<64>
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v7 inplace : !m, i8
  %eq = sea.ptr.cmp eq, %p, %q : !sea.ptr<64>
  %s = sea.select %eq, %q, %r : !sea.ptr<64>
  %x = sea.load %m1[%s] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
