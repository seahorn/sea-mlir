// A-INB under ARM, symbolic offset: p + n with n = -16 (an assume) and p an
// ite over five objects. The offset is not a number, so the access is not a
// definite violation: it is a condition that --seam assumes and counts
// (conditions=1: the answer depends on it). Plain sat; --seam unsat, the
// documented conditional answer (A-INB assumed and reported, not discharged);
// --seam-arm=0 keeps the compare unknown and does not skip across bases, so
// it is sat.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --seam --seam-inb=0 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=COUNT
// RUN: sea-vc %s --lower --seam --seam-arm=0 | FileCheck %s --check-prefix=SAT
// COUNT: seam.inb: assumed accesses=3 conditions=1
// COUNT: result: unsat
// SAT: result: sat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ov {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %o1 = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %o2 = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %o3 = sea.alloca %c16 align 8 [3] : i64 -> !sea.ptr<64>
  %o4 = sea.alloca %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %o5 = sea.alloca %c16 align 8 [5] : i64 -> !sea.ptr<64>
  %e = sea.alloca %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %b1 = sea.nondet "b1" [11] : i1
  %b2 = sea.nondet "b2" [12] : i1
  %b3 = sea.nondet "b3" [13] : i1
  %b4 = sea.nondet "b4" [14] : i1
  %p4 = sea.select %b4, %o4, %o5 : !sea.ptr<64>
  %p3 = sea.select %b3, %o3, %p4 : !sea.ptr<64>
  %p2 = sea.select %b2, %o2, %p3 : !sea.ptr<64>
  %p = sea.select %b1, %o1, %p2 : !sea.ptr<64>
  %n = sea.nondet "n" [15] : i64
  %cm16 = sea.const -16 : i64
  %eqn = sea.icmp eq, %n, %cm16 : i64
  sea.assume %eqn
  %q = sea.ptr.add %p, %n : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %v9 = sea.const 9 : i8
  %m1 = sea.store %m0[%e], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%q], %v9 inplace : !m, i8
  %x = sea.load %m2[%e] : !m -> i8
  %ok = sea.icmp eq, %x, %v1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
