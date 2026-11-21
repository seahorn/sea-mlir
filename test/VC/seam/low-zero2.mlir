// bv.eq.low-zero2: slot[i].f8 is read over a store to slot[prev].f20, prev
// loaded from memory (32-byte slots). The ROW compare of the two addresses
// reduces to 32*zext(i) + (-12) = 32*zext(prev): variables on both sides,
// never true (32 divides both sums, not 12), for every i, prev. The rewriter
// decides it with SeaM's cmp.congruence off, so no such `=` reaches the
// query; also with the default options.
// RUN: sea-vc %s --lower --seam --seam-congruence=false --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-congruence=false --smt2 2>&1 | FileCheck %s --check-prefix=Q
// RUN: sea-vc %s --lower --seam --smt2 2>&1 | FileCheck %s --check-prefix=Q
// RUN: sea-vc %s --lower --seam --seam-congruence=false --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.rule.bv.eq.low-zero2: {{[1-9]}}
// CHECK: result: unsat
// Q-NOT: bvmul
// Q: (check-sat)
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mini {
  %m0 = sea.mem.nondet "region" [0] : !m
  %i = sea.nondet "i" [1] : i32
  %ws = sea.global @"ws" size 304 align 16 : !sea.ptr<64>
  %zi = sea.zext %i : i32 to i64
  %pprev = sea.ptr.index %ws [%zi] strides [32] offset 96 inbounds : !sea.ptr<64>, i64
  %prev = sea.load %m0[%pprev] align 16 : !m -> i32
  %zp = sea.zext %prev : i32 to i64
  %pi8 = sea.ptr.index %ws [%zi] strides [32] offset 88 inbounds : !sea.ptr<64>, i64
  %v0 = sea.load %m0[%pi8] align 8 : !m -> i32
  %pn = sea.ptr.index %ws [%zp] strides [32] offset 100 inbounds : !sea.ptr<64>, i64
  %neg1 = sea.const -1 : i32
  %m1 = sea.store %m0[%pn], %neg1 inplace align 4 : !m, i32
  %v1 = sea.load %m1[%pi8] align 8 : !m -> i32
  %ok = sea.icmp eq, %v0, %v1 : i32
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
