// sea-vc --eq-audit (tools/sea-vc/EqAudit.h): the equalities the rules and
// the Ackermann lowering left undecided, each by the lowest level of facts
// that decides it (L0 none, L1 layout, L2 A-INB, undecided).
// @memset: a read at A + i after memset(A, 7, 16), A of 16 bytes: the rules
// leave the membership (A + i) - A <u 16 open (row.mset), A-INB of the read
// decides it (L2: a missing A-INB rule, found by the audit). The query is
// sat (i >= 16: an out-of-bounds read; A-INB is assumed by the rules, not
// added to the query).
// @reads: two reads of the initial memory at A + i and A + j: one Ackermann
// pair, undecided at every level (the solver's search).
// RUN: sea-vc %s --program=memset --lower --seam --eq-audit 2>&1 | FileCheck %s --check-prefix=MEMSET
// --vcgen-only: no solve, the audit still runs (its own z3 checks)
// RUN: sea-vc %s --program=memset --lower --seam --eq-audit --vcgen-only 2>&1 | FileCheck %s --check-prefix=MEMSET
// --seam-check comes after a solve: under --vcgen-only, a warning
// RUN: sea-vc %s --program=memset --lower --seam --seam-check --vcgen-only 2>&1 | FileCheck %s --check-prefix=SKIP
// SKIP: [vc.vcgen-only-check-skipped]
// RUN: %if yices %{ sea-vc %s --program=reads --lower --seam --eq-audit --solver=yices 2>&1 | FileCheck %s --check-prefix=READS %}
// MEMSET: eq-audit: source=row-memop items=1 sampled=1 L0=0 L1=0 L2=1 undecided=0
// MEMSET: eq-audit-shape: source=row-memop level=L2 count=1 {{.*}} shape=(bvult (bvsub (bvadd O X) O) K)
// MEMSET-NEXT: eq-audit-example: bvult(
// READS: eq-audit: source=ack items=1 sampled=1 L0=0 L1=0 L2=0 undecided=1
// READS: eq-audit-shape: source=ack level=undecided count=1 {{.*}} shape=(bvadd O X) | (bvadd O X)
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @memset {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %v7 = sea.const 7 : i8
  %m1 = sea.memset %m0[%A], %v7, %c16 inplace : !m
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %x = sea.load %m1[%p] : !m -> i8
  %ok = sea.icmp eq, %x, %v7 : i8
  sea.assert %ok
  sea.halt
}
sea.program @reads {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8
  %y = sea.load %m0[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
