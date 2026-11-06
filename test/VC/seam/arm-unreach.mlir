// A-INB under ARM, a leaf that cannot be selected: the sum p + 20 over an ite
// of five objects has an entry (o1, [20, 20]) wholly outside o1 (16 bytes),
// but the o1 leaf is selected only when b1 holds and the program assumes !b1.
// Guarded aggregation (docs/design/SeaM-null.md, point 3): the o1 provenance
// is `false` in the access condition, the others are live, so the access is
// not a definite violation: no fallback, A-INB assumed (before: a whole-run
// fallback, imprecise because the leaf is unreachable). Unsat either way.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=GUARDED
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHECK-FIRINGS
// GUARDED-NOT: vc.seam-inb-violated
// GUARDED: seam.inb: assumed
// GUARDED: result: unsat
// UNSAT: result: unsat
// CHECK-FIRINGS: result: unsat
// CHECK-FIRINGS: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unreach {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %c32 = sea.const 32 : i64
  %o1 = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %o2 = sea.alloca %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %o3 = sea.alloca %c32 align 8 [3] : i64 -> !sea.ptr<64>
  %o4 = sea.alloca %c32 align 8 [4] : i64 -> !sea.ptr<64>
  %o5 = sea.alloca %c32 align 8 [5] : i64 -> !sea.ptr<64>
  %e = sea.alloca %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %f = sea.alloca %c16 align 8 [7] : i64 -> !sea.ptr<64>
  %b1 = sea.nondet "b1" [11] : i1
  %b2 = sea.nondet "b2" [12] : i1
  %b3 = sea.nondet "b3" [13] : i1
  %b4 = sea.nondet "b4" [14] : i1
  %t = sea.const 1 : i1
  %nb1 = sea.xor %b1, %t : i1
  sea.assume %nb1
  %p4 = sea.select %b4, %o4, %o5 : !sea.ptr<64>
  %p3 = sea.select %b3, %o3, %p4 : !sea.ptr<64>
  %p2 = sea.select %b2, %o2, %p3 : !sea.ptr<64>
  %p = sea.select %b1, %o1, %p2 : !sea.ptr<64>
  %c20 = sea.const 20 : i64
  %q = sea.ptr.add %p, %c20 : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %v9 = sea.const 9 : i8
  %m1 = sea.store %m0[%e], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%f], %v9 inplace : !m, i8
  %m3 = sea.store %m2[%q], %v9 inplace : !m, i8
  %x = sea.load %m3[%e] : !m -> i8
  %ok = sea.icmp eq, %x, %v1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
