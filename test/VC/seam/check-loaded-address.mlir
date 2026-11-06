// Sibling of check-loaded-length.mlir: the destination offset of a memmove
// is loaded from memory written by a memset term, so a firing's
// destination-range premise holds that read; --seam-check translates the
// premise through the semantics-only boundary.
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --seam-row-expand=0 --seam-check | FileCheck %s %}
// RUN: sea-vc %s --lower --seam --solver=z3 --seam-row-expand=0 --seam-check | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --seam-check | FileCheck %s %}
// CHECK: result: unsat
// CHECK: seam-check: {{.*}}invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @addr {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %idx = sea.nondet "idx" [3] : i64
  %c4 = sea.const 4 : i64
  %iok = sea.icmp ult, %idx, %c4 : i64
  sea.assume %iok
  // a[0 .. 4) <- 16: the loaded offset is 16
  %v16 = sea.const 16 : i8
  %mi = sea.memset %m0[%a], %v16, %c4 inplace : !m
  %pp = sea.ptr.add %a, %idx : !sea.ptr<64>, i64
  %ob = sea.load %mi[%pp] : !m -> i8
  %off = sea.zext %ob : i8 to i64
  %dst = sea.ptr.add %a, %off : !sea.ptr<64>, i64
  %c2 = sea.const 2 : i64
  %pb = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %xb = sea.load %mi[%pb] : !m -> i8
  %xb0 = sea.load %mi[%b] : !m -> i8
  // a[off .. off + 8) <- b[0 .. 8)
  %c8 = sea.const 8 : i64
  %m1 = sea.memmove %mi[%dst], %mi[%b], %c8 inplace : !m
  %yb = sea.load %m1[%pb] : !m -> i8
  %c1 = sea.const 1 : i64
  %pa1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %ya = sea.load %m1[%pa1] : !m -> i8
  %c16 = sea.const 16 : i64
  %pa16 = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64
  %y16 = sea.load %m1[%pa16] : !m -> i8
  %e1 = sea.icmp eq, %yb, %xb : i8
  %e2 = sea.icmp eq, %ya, %v16 : i8
  %e3 = sea.icmp eq, %y16, %xb0 : i8
  %e12 = sea.and %e1, %e2 : i1
  %e = sea.and %e12, %e3 : i1
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
