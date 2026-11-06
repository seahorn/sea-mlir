// --seam-check translates each firing's premise through the semantics-only
// boundary, like its two sides: here a destination-range premise holds a
// length loaded from memory written by a memset term. This premise reached
// the solver with that term inside and caused an abort in the Yices encoder.
// Row expansion off keeps the
// read over the term in the premise.
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --seam-row-expand=0 --seam-check | FileCheck %s %}
// RUN: sea-vc %s --lower --seam --solver=z3 --seam-row-expand=0 --seam-check | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --seam-check | FileCheck %s %}
// CHECK: result: unsat
// CHECK: seam-check: {{.*}}invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @terms {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %idx = sea.nondet "idx" [3] : i64
  %ibound = sea.const 8 : i64
  %iok = sea.icmp ult, %idx, %ibound : i64
  sea.assume %iok
  %initlen = sea.const 4 : i64
  %initval = sea.const 2 : i8
  %mi = sea.memset %m0[%a], %initval, %initlen inplace : !m
  %pp = sea.ptr.add %a, %idx : !sea.ptr<64>, i64
  %nb = sea.load %mi[%pp] : !m -> i8
  %n = sea.zext %nb : i8 to i64
  %c2 = sea.const 2 : i64
  %c8 = sea.const 8 : i64
  %ge = sea.icmp uge, %n, %c2 : i64
  sea.assume %ge
  %le = sea.icmp ule, %n, %c8 : i64
  sea.assume %le
  %k = sea.nondet "k" [4] : i64
  %c16 = sea.const 16 : i64
  %kle = sea.icmp ult, %k, %c16 : i64
  sea.assume %kle
  %pb = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %xb = sea.load %mi[%pb] : !m -> i8
  %c1 = sea.const 1 : i64
  %pa1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %pa16 = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64
  %z = sea.const 0 : i8
  // a[16 .. 16 + n) <- b[0 .. n); then a[0 .. 8) <- 0
  %m1 = sea.memmove %mi[%pa16], %mi[%b], %n inplace : !m
  %m2 = sea.memset %m1[%a], %z, %c8 inplace : !m
  %yb = sea.load %m2[%pb] : !m -> i8
  %ya = sea.load %m2[%pa1] : !m -> i8
  %pk = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %yk = sea.load %m2[%pk] : !m -> i8
  %e1 = sea.icmp eq, %yb, %xb : i8
  %e2 = sea.icmp eq, %ya, %z : i8
  %e12 = sea.and %e1, %e2 : i1
  // k < 8: a + k was set to 0
  %kin = sea.icmp uge, %k, %c8 : i64
  %e3 = sea.icmp eq, %yk, %z : i8
  %e4 = sea.or %kin, %e3 : i1
  %e = sea.and %e12, %e4 : i1
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
