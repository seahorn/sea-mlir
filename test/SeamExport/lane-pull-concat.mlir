// sea-seam-export, lane seam-inb0-pull-concat (sea-vc --seam --seam-inb=0
// --seam-pull-concat=4): a split pointer load whose bytes share an ite
// condition. A pointer to x or the integer n is written byte by byte under
// the same condition c and reloaded as two byte loads, so the reloaded
// pointer is from_bits(concat(ite(c, x1, n1), ite(c, x0, n0))). The lane
// pulls c out of the concat and the then side reassembles x: the firing
// ptr.load-resolve-concat. Lane seam-inb0 (no pulls) leaves the concat.
// RUN: sea-seam-export %s --address-bits=16 --lane=seam-inb0-pull-concat --firings | %python %S/Inputs/show.py | FileCheck %s --check-prefixes=CHECK,PULL
// RUN: sea-seam-export %s --address-bits=16 --lane=seam-inb0 --firings | %python %S/Inputs/show.py | FileCheck %s --check-prefixes=CHECK,NOPULL
// PULL: header: schema=2 lane=seam-inb0-pull-concat sea_vc='sea-vc --seam --seam-inb=0 --seam-pull-concat=4'
// NOPULL: header: schema=2 lane=seam-inb0 sea_vc='sea-vc --seam --seam-inb=0'
// PULL: root %{{[0-9]+}} sea.addr.from_bits ptr: ite(eq(nd.1_0, 1), obj.x, fromBits(nd.2_0))
// NOPULL: root %{{[0-9]+}} sea.addr.from_bits ptr: fromBits(concat(ite(
// PULL: firing {{[0-9]+}} rules=ptr.load-resolve-concat premises=none op=query
// NOPULL-NOT: ptr.load-resolve-concat
// CHECK: firings: {{[0-9]+}} unexpressible=0
!m = !sea.mem<!sea.addr<16>, i8>
sea.program @split {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c = sea.nondet "c" [1] : i1
  %n = sea.nondet "n" [2] : i16
  %t = sea.segment "t" base 256 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %x = sea.segment "x" base 512 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %xb = sea.addr.to_bits %x : !sea.addr<16> to i16
  %x0 = sea.extract %xb[0] : i16 -> i8
  %x1 = sea.extract %xb[8] : i16 -> i8
  %n0 = sea.extract %n[0] : i16 -> i8
  %n1 = sea.extract %n[8] : i16 -> i8
  %b0 = sea.select %c, %x0, %n0 : i8
  %b1 = sea.select %c, %x1, %n1 : i8
  %one = sea.const 1 : i16
  %t1 = sea.addr.add %t, %one : !sea.addr<16>, i16
  %m1 = sea.store %m0[%t], %b0 : !m, i8
  %m2 = sea.store %m1[%t1], %b1 : !m, i8
  %l0 = sea.load %m2[%t] : !m -> i8
  %l1 = sea.load %m2[%t1] : !m -> i8
  %w = sea.concat %l1, %l0 : i8, i8 -> i16
  %q = sea.addr.from_bits %w : i16 to !sea.addr<16>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
