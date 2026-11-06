// Null-object acceptance, row 4 (docs/design/SeaM-null.md, point 5), as
// specified by the tracking contract ("Shadow memories"):
// objects of size 0 (malloc(0)) keep size 0 but have their own base
// (sea-place-static reserves one unit), so p, r and q below are at three
// addresses. Pointer equality stays numeric (no rule decides a program
// compare by base): p == r and p == q are false numerically, as in the
// abstract interpreter (@eq: unsat with and without --seam; the interpreter
// halts). A positive-width access through p is infeasible under A-INB
// (@access): the store through ite(c, p, A) cannot be in p
// (cmp.size0-vacuous) nor reach q from A (cmp.diff-base), so the read of q
// skips it: unsat under --seam, conditional on A-INB. Plain is also unsat:
// a store through p writes p's own unit, not q's. The interpreter reports the store
// through p (c = 1) as an out-of-bounds access: the safety job's violation.
// With addr.prune-size0 (docs/design/SeaM-provenance.md, default
// on) the store address drops its p leaf first (a zero-sized allocation is
// pruned as null is); the compare path is pinned with --seam-prune-size0=0.
// RUN: sea-vc %s --lower --program=eq | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=eq --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: %if yices %{ sea-vc %s --lower --program=eq --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK %}
// RUN: sea-run %s --program=eq | FileCheck %s --check-prefix=INTERP-EQ
// RUN: sea-vc %s --lower --program=access | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=access --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=ACC
// RUN: sea-vc %s --lower --program=access --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: %if yices %{ sea-vc %s --lower --program=access --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK %}
// RUN: sea-vc %s --lower --program=access --seam --seam-prune-size0=0 --seam-stats 2>&1 | FileCheck %s --check-prefix=NOPRUNE
// RUN: sea-vc %s --lower --program=access --seam --seam-prune-size0=0 --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: not sea-run %s --program=access --input=5=1 --missing=zero | FileCheck %s --check-prefix=ACC-P
// RUN: sea-run %s --program=access --input=5=0 --missing=zero | FileCheck %s --check-prefix=ACC-A
// UNSAT: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// INTERP-EQ: status: halted
// ACC-NOT: vc.seam-inb-violated
// ACC: seam.inb: assumed
// ACC-DAG: seam.rule.addr.prune-size0: {{[1-9]}}
// ACC-DAG: seam.rule.cmp.diff-base: {{[1-9]}}
// ACC: result: unsat
// NOPRUNE-NOT: seam.rule.addr.prune-size0
// NOPRUNE-DAG: seam.rule.cmp.size0-vacuous: {{[1-9]}}
// NOPRUNE-DAG: seam.rule.cmp.diff-base: {{[1-9]}}
// NOPRUNE: result: unsat
// ACC-P: status: error
// ACC-A: status: halted
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @eq {
  %c0 = sea.const 0 : i64
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c0 align 4 [1] : i64 -> !sea.ptr<64>
  %r = sea.malloc %c0 align 4 [2] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c4 align 4 [3] : i64 -> !sea.ptr<64>
  %e1 = sea.ptr.cmp eq, %p, %r : !sea.ptr<64>
  %e2 = sea.ptr.cmp eq, %p, %q : !sea.ptr<64>
  %both = sea.and %e1, %e2 : i1
  %t = sea.const 1 : i1
  %ok = sea.xor %both, %t : i1
  sea.assert %ok
  sea.halt
}
sea.program @access {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c0 = sea.const 0 : i64
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c0 align 4 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c4 align 4 [3] : i64 -> !sea.ptr<64>
  %A = sea.malloc %c4 align 4 [4] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [5] : i1
  %s = sea.select %c, %p, %A : !sea.ptr<64>
  %y = sea.load %m0[%q] : !m -> i8
  %v5 = sea.const 5 : i8
  %m1 = sea.store %m0[%s], %v5 inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
