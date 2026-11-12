// ptr.overlap.cancel-num: a pointer stored at q + 8 and loaded at p + 8 in
// byte mode (no --seam-wide), p and q loaded pointers (no object, no view). The
// byte fallback asks overlap(q+8 [K=k], 1, p+8 [K=k], 1) for byte k of the
// load against byte k of the store, eight terms that differ only in K. The
// common numeral cancels (addition mod 2^64; no assumption): every one is
// overlap(q+8, 1, p+8, 1), one alias question (an IS_OVERLAP, not a PTR_EQ). The byte pairs k != k'
// (partial overlaps of misaligned pointers) cancel to one term per
// difference k - k' (14): 15 open questions, 64 without the rule.
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.alias.open.all: 15
// CHECK: seam.rule.ptr.overlap.cancel-num: {{[1-9]}}
// CHECK: result: unsat
// CHK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @kcancel {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %H = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %c8 = sea.const 8 : i64
  %X = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %H8 = sea.ptr.add %H, %c8 : !sea.ptr<64>, i64
  %P = sea.load %m0[%H] : !m -> !sea.ptr<64>
  %Q = sea.load %m0[%H8] : !m -> !sea.ptr<64>
  %p8 = sea.ptr.index %P [] strides [] offset 8 inbounds : !sea.ptr<64>
  %q8 = sea.ptr.index %Q [] strides [] offset 8 inbounds : !sea.ptr<64>
  %m1 = sea.store %m0[%q8], %X inplace : !m, !sea.ptr<64>
  %r = sea.load %m1[%p8] : !m -> !sea.ptr<64>
  // q == p: the load is X
  %same = sea.ptr.cmp eq, %P, %Q : !sea.ptr<64>
  %eq = sea.ptr.cmp eq, %r, %X : !sea.ptr<64>
  %t1 = sea.const 1 : i1
  %ns = sea.xor %same, %t1 : i1
  %ok = sea.or %ns, %eq : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
