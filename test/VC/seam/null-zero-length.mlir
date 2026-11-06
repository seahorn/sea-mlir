// Null-object acceptance, row 3 (docs/design/SeaM-null.md, point 6): a range
// of length 0 accesses nothing, even through null. memset(p, v, n) with
// p = ite(c, null, A) and a symbolic n <= 1, then a read of B: the range
// premise is ite(c, n = 0, n <= 16): n = 0 through null is admitted, n = 1
// through null violates it. The read skips the memset (null leaf:
// cmp.size0-vacuous, A leaf: cmp.diff-base) under that satisfiable premise
// (checked by --seam-check through the semantics-only boundary). Unsat with
// and without --seam. The interpreter: n = 0 through null halts, n = 1 is a
// null dereference.
// With addr.prune-size0 (docs/design/SeaM-provenance.md, default
// on) the null leaf of the destination is dropped first
// (addr.prune-size0-range): an operation-level equivalence, checked by
// --seam-check as equal memories under the original range premise, which
// admits n = 0 with c true (both memsets no-ops, addresses different); then
// cmp.diff-base decides the one pair left. The compare path is pinned with
// --seam-prune-size0=0.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --seam --seam-prune-size0=0 --seam-stats 2>&1 | FileCheck %s --check-prefix=NOPRUNE
// RUN: sea-vc %s --lower --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --seam --seam-prune-size0=0 --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: sea-run %s --input=3=1,4=0 --missing=zero | FileCheck %s --check-prefix=N0
// RUN: not sea-run %s --input=3=1,4=1 --missing=zero | FileCheck %s --check-prefix=N1
// PLAIN: result: unsat
// SEAM-NOT: vc.seam-inb-violated
// SEAM: seam.inb: assumed
// SEAM-DAG: seam.rule.addr.prune-size0-range: {{[1-9]}}
// SEAM-DAG: seam.rule.row.mset-skip: {{[1-9]}}
// SEAM: result: unsat
// NOPRUNE-NOT: seam.rule.addr.prune-size0
// NOPRUNE-DAG: seam.rule.cmp.size0-vacuous: {{[1-9]}}
// NOPRUNE-DAG: seam.rule.row.mset-skip: {{[1-9]}}
// NOPRUNE: result: unsat
// CHK-NOT: vc.seam-inb-violated
// CHK: result: unsat
// CHK: seam-check: firings={{[1-9][0-9]*}} {{.*}} invalid=0 unknown=0 unexpressible=0
// N0: status: halted
// N1: status: error
// N1: message: memory access through the null pointer
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @zerolen {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %n = sea.nondet "n" [4] : i64
  %one = sea.const 1 : i64
  %nok = sea.icmp ule, %n, %one : i64
  sea.assume %nok
  %null = sea.ptr.null : !sea.ptr<64>
  %p = sea.select %c, %null, %A : !sea.ptr<64>
  %v7 = sea.const 7 : i8
  %y = sea.load %m0[%B] : !m -> i8
  %m1 = sea.memset %m0[%p], %v7, %n inplace : !m
  %x = sea.load %m1[%B] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
