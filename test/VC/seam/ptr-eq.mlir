// PtrSort S2d: a pointer compare eq / ne (sea.addr.cmp after placement) is pointer equality (PTR_EQ, ne as its
// negation), erased to = at the boundary. @same: p = A + i and q = A + j
// (inbounds) are equal iff i = j: the compare reduces to the offsets
// (cmp.same-base). @ne: p != q is the negation. The solver sees no ptr-eq.
// RUN: sea-vc %s --lower --program=same --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,SAME
// RUN: sea-vc %s --lower --program=ne --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,SAME
// RUN: sea-vc %s --lower --program=same --seam --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: sea-vc %s --lower --program=ne --seam --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --program=same 2>&1 | FileCheck %s --check-prefix=UNSAT
// SAME: seam.rule.cmp.same-base: {{[1-9]}}
// UNSAT: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// SMT-NOT: ptr-eq
// SMT: (check-sat)
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @same {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %p = sea.ptr.index %A [%i] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %q = sea.ptr.index %A [%j] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %e = sea.ptr.cmp eq, %p, %q : !sea.ptr<64>
  %ij = sea.icmp eq, %i, %j : i64
  %ok = sea.icmp eq, %e, %ij : i1
  sea.assert %ok
  sea.halt
}
sea.program @ne {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %p = sea.ptr.index %A [%i] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %q = sea.ptr.index %A [%j] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %e = sea.ptr.cmp ne, %p, %q : !sea.ptr<64>
  %ij = sea.icmp ne, %i, %j : i64
  %ok = sea.icmp eq, %e, %ij : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
