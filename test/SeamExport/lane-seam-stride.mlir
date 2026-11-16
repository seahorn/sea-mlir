// sea-seam-export, lane seam-stride (sea-vc --seam --seam-stride): R-STRIDE
// (docs/design/PtrIndex.md section 0c). Two numeral store-map entries at
// offsets 100 and 130 of an object, read through the GEP a + 3 i (nusw):
// 3 divides neither offset, so the read skips both entries
// (row.smap-abort-stride, decided by cmp.stride) under the GEP's promise
// (premise PTR-INDEX). Lane seam (no R-STRIDE) keeps the entries. The exit
// status is not pinned: the promise term may have no node in the format
// (an unexpressible firing, status 3); the firing is recorded either way.
// RUN: sea-seam-export %s --lane=seam-stride --firings > %t.stride || true
// RUN: %python %S/Inputs/show.py < %t.stride | FileCheck %s --check-prefixes=CHECK,STRIDE
// RUN: sea-seam-export %s --lane=seam --firings | %python %S/Inputs/show.py | FileCheck %s --check-prefixes=CHECK,NOSTRIDE
// STRIDE: header: schema=3 lane=seam-stride sea_vc='sea-vc --seam --seam-stride' {{.*}}ainb=true
// NOSTRIDE: header: schema=3 lane=seam sea_vc='sea-vc --seam' {{.*}}ainb=true
// STRIDE: firing {{[0-9]+}} rules={{[^ ]*}}row.smap-abort-stride{{[^ ]*}} premises={{[^ ]*}}PTR-INDEX
// NOSTRIDE-NOT: row.smap-abort-stride
// CHECK: firings: {{[0-9]+}}
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
