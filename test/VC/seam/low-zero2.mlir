// Table slots indexed by a loaded value (reduced from a firedancer
// reachability job). Slots of 32 bytes; prev is loaded from slot[i]; -1 is
// stored at slot[prev] + 20, then slot[i] + 8 is read again. The ROW compare
// of the read over the store, 32i + 8 vs 32 zext(prev) + 20, is never true
// (32 divides the variable part, not 12), for every i and prev; ARM gives
// prev no range. Three ways it is decided:
// - default: SeaM's cmp.congruence decides it, the store is skipped
//   (row.store-skip), no compare is left;
// - --seam-congruence=false: the rewriter's bv.eq.low-zero2 decides it once
//   it is linear (32*zext(i) + (-12) = 32*zext(prev), variables on both
//   sides), so no such `=` reaches the query;
// - --seam-congruence=false --seam-simplify=0: nothing decides it; the
//   store stays (row.store) and an alias question is left open.
// The verdict is unsat in all three.
// RUN: sea-vc %s --lower --seam --widen-memory=4 --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,DEF
// RUN: sea-vc %s --lower --seam --widen-memory=4 --seam-stats --seam-congruence=false 2>&1 | FileCheck %s --check-prefixes=CHECK,LZ
// RUN: sea-vc %s --lower --seam --widen-memory=4 --seam-stats --seam-congruence=false --seam-simplify=0 2>&1 | FileCheck %s --check-prefixes=CHECK,RAW
// RUN: sea-vc %s --lower --seam --seam-congruence=false --smt2 2>&1 | FileCheck %s --check-prefix=Q
// RUN: sea-vc %s --lower --seam --smt2 2>&1 | FileCheck %s --check-prefix=Q
// RUN: sea-vc %s --lower --seam --seam-congruence=false --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// DEF-NOT: seam.alias.open
// DEF: seam.rule.cmp.congruence: {{[1-9]}}
// DEF-NOT: seam.rule.row.store:
// DEF: seam.rule.row.store-skip: {{[1-9]}}
// LZ-NOT: cmp.congruence
// LZ: seam.rule.bv.eq.low-zero2: {{[1-9]}}
// RAW: seam.alias.open.same-object: {{[1-9]}}
// RAW-NOT: cmp.congruence
// RAW-NOT: bv.eq.low-zero2
// RAW: seam.rule.row.store: {{[1-9]}}
// RAW-NOT: seam.rule.row.store-skip
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
