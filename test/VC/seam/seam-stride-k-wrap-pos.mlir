// RUN: sea-vc %s --lower --seam --seam-stats --seam-stride --seam-check | FileCheck %s

// R-STRIDE with K > 0 (docs/design/PtrIndex.md section 0c): the nuw node
// N = a[3 i] at b + O = 1 + 252 = 253 (i = 84, P3 holds); a program
// addr.add of 4 writes K = 4, and the 8-byte load at N + 4 reads bytes
// 257.. = 1.. (mod 256) = a + 0.., inside heap.1 = [1, 101): A-INB holds,
// and byte 0 reads the entry a + 0 (7). 3 does not divide 0 - 4, but the
// integer address wraps. K > b = 1 for every byte (K = 4..11, also below
// the access width 8 for K = 4..7), so no entry is skipped: sat.
// CHECK-NOT: smap-abort-stride
// CHECK: result: sat
// CHECK: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @posk {
  %a = sea.segment "heap.1" base 1 : i8 size 100 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %v7 inplace : !am, i8
  %o2 = sea.const 50 : i8
  %p2 = sea.addr.add %a, %o2 : !sea.addr<8>, i8
  %m2 = sea.store %m1[%p2], %v7 inplace : !am, i8
  %i = sea.nondet "i" [1] : i8
  %c84 = sea.const 84 : i8
  %ie = sea.icmp eq, %i, %c84 : i8
  sea.assume %ie
  %n = sea.ptr.index %a [%i] strides [3] offset 0 nuw : !sea.addr<8>, i8
  %k = sea.const 4 : i8
  %q = sea.addr.add %n, %k : !sea.addr<8>, i8
  %x = sea.load %m2[%q] : !am -> i64
  %z64 = sea.const 0 : i64
  %ok = sea.icmp eq, %x, %z64 : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
