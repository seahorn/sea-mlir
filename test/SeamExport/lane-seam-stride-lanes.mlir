// sea-seam-export, lanes seam-inb0-stride (sea-vc --seam --seam-premises=0
// --seam-stride) and seam-wide-stride (sea-vc --seam --seam-wide
// --seam-stride): R-STRIDE fires in both (program of lane-seam-stride.mlir).
// RUN: sea-seam-export %s --lane=seam-inb0-stride --firings | %python %S/Inputs/show.py | FileCheck %s --check-prefixes=INB0
// RUN: sea-seam-export %s --lane=seam-wide-stride --firings | %python %S/Inputs/show.py | FileCheck %s --check-prefixes=WIDE
// INB0: header: schema=3 lane=seam-inb0-stride sea_vc='sea-vc --seam --seam-premises=0 --seam-stride' {{.*}}ainb=false
// INB0: firing {{[0-9]+}} rules={{[^ ]*}}row.smap-abort-stride{{[^ ]*}} {{.*}}op=load
// WIDE: header: schema=3 lane=seam-wide-stride sea_vc='sea-vc --seam --seam-wide --seam-stride' {{.*}}ainb=true
// WIDE: firing {{[0-9]+}} rules={{[^ ]*}}row.smap-abort-stride{{[^ ]*}} {{.*}}op=load
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @stride {
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
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
