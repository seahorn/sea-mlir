// PtrSort S2b: sameAddr compares canonical forms (canon: nodes unflagged,
// chains merged, numerals folded, pairs sorted). Over a loaded pointer p
// (no object, no view):
// @promise: a store at p + 4i with the inbounds promise and a load at the
// same address without it are one address (cmp.refl), no assumption.
// @chain: an unflagged node over an unflagged node is one node
// (ptrindex.merge); the load at (p + 4i) + j hits the store at p + 4i + j
// written the other way round (pairs sorted).
// @plain: a read at A + 8 by ptr.add (a flat sum) and one at the inbounds
// node A + 8 are one address: the ite of the two reads is one read
// (ite.same through canon's pointer sums).
// RUN: sea-vc %s --lower --program=promise --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,PROM
// RUN: sea-vc %s --lower --program=chain --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHAIN
// RUN: sea-vc %s --lower --program=plain --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,PLAIN
// RUN: sea-vc %s --lower --program=plain --seam --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: sea-vc %s --lower --program=promise --seam --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: sea-vc %s --lower --program=chain --seam --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// PROM: seam.rule.cmp.refl: {{[1-9]}}
// CHAIN: seam.rule.ptrindex.merge: {{[1-9]}}
// PLAIN: seam.rule.ite.same: {{[1-9]}}
// UNSAT: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @promise {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %cell = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %p = sea.load %m0[%cell] : !m -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %v = sea.nondet "v" [3] : i8
  %a1 = sea.ptr.index %p [%i] strides [4] offset 0 inbounds : !sea.ptr<64>, i64
  %a2 = sea.ptr.index %p [%i] strides [4] offset 0 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%a1], %v inplace align 1 : !m, i8
  %r = sea.load %m1[%a2] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}
sea.program @chain {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %cell = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %p = sea.load %m0[%cell] : !m -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %v = sea.nondet "v" [4] : i8
  %b1 = sea.ptr.index %p [%i] strides [4] offset 0 : !sea.ptr<64>, i64
  %a1 = sea.ptr.index %b1 [%j] strides [1] offset 0 : !sea.ptr<64>, i64
  %b2 = sea.ptr.index %p [%j] strides [1] offset 0 : !sea.ptr<64>, i64
  %a2 = sea.ptr.index %b2 [%i] strides [4] offset 0 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%a1], %v inplace align 1 : !m, i8
  %r = sea.load %m1[%a2] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}
sea.program @plain {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [2] : i1
  %a1 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %a2 = sea.ptr.index %A [] strides [] offset 8 inbounds : !sea.ptr<64>
  %v1 = sea.load %m0[%a1] align 1 : !m -> i8
  %v2 = sea.load %m0[%a2] align 1 : !m -> i8
  %s = sea.select %c, %v1, %v2 : i8
  %ok = sea.icmp eq, %s, %v1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
