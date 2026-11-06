// RUN: sea-vc %s --seam --seam-stats --seam-stride --seam-check | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-stats --seam-stride --seam-check | FileCheck %s

// R-STRIDE with K != 0 (docs/design/PtrIndex.md section 0c): a program
// addr.add of a numeral to a promised node writes K signed, and A-INB of the
// access bounds its address modulo 2^n only. Two program addr.adds of -100
// to N = a[3 i] (i = 0) write K = -200; the access at 16 - 200 = 72
// (mod 256) = a + 56 is in heap.1 = [16, 116) and reads the entry a + 56
// (7). 3 does not divide 56 - (-200) = 256, but the integer address wraps:
// the true offset is O + K + 256 = 56. K is outside [b + size - 2^n, b] =
// [-140, 16], so the entry is not skipped and the answer is sat.
// CHECK-NOT: smap-abort-stride
// CHECK: result: sat
// CHECK: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @negk {
  %a = sea.segment "heap.1" base 16 : i8 size 100 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %o = sea.const 56 : i8
  %p = sea.addr.add %a, %o : !sea.addr<8>, i8
  %m1a = sea.store %m0[%p], %v7 inplace : !am, i8
  %o2 = sea.const 1 : i8
  %p2 = sea.addr.add %a, %o2 : !sea.addr<8>, i8
  %m1 = sea.store %m1a[%p2], %v7 inplace : !am, i8
  %i = sea.nondet "i" [1] : i8
  %zi = sea.const 0 : i8
  %ie = sea.icmp eq, %i, %zi : i8
  sea.assume %ie
  %n = sea.ptr.index %a [%i] strides [3] offset 0 nusw : !sea.addr<8>, i8
  %k = sea.const -100 : i8
  %q1 = sea.addr.add %n, %k : !sea.addr<8>, i8
  %q = sea.addr.add %q1, %k : !sea.addr<8>, i8
  %x = sea.load %m1[%q] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
