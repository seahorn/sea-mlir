// RUN: sea-vc %s --seam --seam-stats --seam-stride --seam-check | FileCheck %s

// The --seam-check premise of a K = 0 R-STRIDE firing includes the
// placement equations (docs/design/PtrIndex.md section 0c, premise.layout):
// the entry a + 130 has o = 130 >= 2^(n-1), and the nusw stride 3 does not
// divide 130 but divides 130 - 256. The integer reading is exact only with
// the layout numeral (b = 1: b + o < 256), not under P1-P3 alone.
// CHECK: seam.rule.premise.layout: 2
// CHECK: seam.rule.row.smap-abort-stride: 2
// CHECK: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @lay {
  %a = sea.segment "heap.1" base 1 : i8 size 200 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %o = sea.const 130 : i8
  %p = sea.addr.add %a, %o : !sea.addr<8>, i8
  %m1a = sea.store %m0[%p], %v7 inplace : !am, i8
  %o2 = sea.const 100 : i8
  %p2 = sea.addr.add %a, %o2 : !sea.addr<8>, i8
  %m1 = sea.store %m1a[%p2], %v7 inplace : !am, i8
  %i = sea.nondet "i" [1] : i8
  %q = sea.ptr.index %a [%i] strides [3] offset 0 nusw : !sea.addr<8>, i8
  %x = sea.load %m1[%q] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
