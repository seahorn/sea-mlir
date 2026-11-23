// sea-seam-export, lanes seam-wide and seam-wide-stride: the ALIGN premise
// of an aligned wide access. W-ROW-ALIGNED decides a 2-byte read at a + j
// over a 2-byte write at a + k (both `align 2`) as an overlap or a miss,
// relying on the addresses' low bit being 0: the premise holds
// eq(bvand(addr(p), 1), 0) per access (bvand with a numeral mask, `al - 1`).
// RUN: sea-seam-export %s --lane=seam-wide --address-bits=16 --firings | %python %S/Inputs/show.py | FileCheck %s
// RUN: sea-seam-export %s --lane=seam-wide-stride --address-bits=16 --firings | %python %S/Inputs/show.py | FileCheck %s
// CHECK: firing 0 rules=wide.row-aligned premises=ALIGN op=load units=2
// CHECK-NEXT: lhs: wselect(wstore(constMem(0), index(segment(obj.a, 16), nd.2_0*1, c=0, K=0, plain), nd.1_0, bytes=2, align=2), index(segment(obj.a, 16), nd.3_0*1, c=0, K=0, plain), bytes=2, align=2)
// CHECK-NEXT: rhs: ite(unitOverlap(
// CHECK-NEXT: premise: and(true, eq(bvand(addr(index(segment(obj.a, 16), nd.3_0*1, c=0, K=0, plain)), 1), 0), eq(bvand(addr(index(segment(obj.a, 16), nd.2_0*1, c=0, K=0, plain)), 1), 0))
// CHECK: firings: {{[1-9][0-9]*}} unexpressible=0
!am = !sea.mem<!sea.addr<16>, i8>
sea.program @wide {
  %a = sea.segment "a" base 16 : i16 size 8 : i16 align 8 kind "stack" : !sea.addr<16>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v = sea.nondet "v" [1] : i16
  %k = sea.nondet "k" [2] : i16
  %j = sea.nondet "j" [3] : i16
  %ak = sea.addr.add %a, %k : !sea.addr<16>, i16
  %aj = sea.addr.add %a, %j : !sea.addr<16>, i16
  %m1 = sea.store %m0[%ak], %v inplace align 2 : !am, i16
  %x = sea.load %m1[%aj] align 2 : !am -> i16
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
