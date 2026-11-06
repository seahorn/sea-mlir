// Memory-operation terms (docs/design/SeaM-memops.md, step 1): with --seam a
// memmove / memset / memhavoc of a byte memory is one term (mcopy / mset /
// mhavoc), whatever its length; sea-expand-memset does not run (no
// memset.symbolic-bound, no bound on n). A read of b over the copy into a and
// the memset of a is a cross-object skip (A-INB: the whole destination range,
// premise.a-inb); the read of a at a + 1 over memset(a, 0, 8) is a hit; the
// read of a + k is undecided (one ite, one compare) over both terms.
// No term reaches the solver (vc.dag-op has no mcopy / mset / mhavoc). With
// the rewriting off or interrupted the boundary translates the reads by their
// equations (memop.translated > 0), same verdict; every firing is valid.
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --seam --dag-stats 2>&1 | FileCheck %s --check-prefix=DAG
// RUN: sea-vc %s --lower --seam --seam-stats --seam-row-expand=0 2>&1 | FileCheck %s --check-prefix=ROW0
// RUN: sea-vc %s --lower --seam --seam-rule-limit=2 2>&1 | FileCheck %s --check-prefix=LIMIT
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHECK1
// RUN: %if yices %{ sea-vc %s --lower --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHECK1 %}
// RUN: sea-vc %s --lower 2>&1 | FileCheck %s --check-prefix=PLAIN
// SEAM-NOT: memset.symbolic-bound
// SEAM-DAG: seam.rule.row.mcopy-skip:
// SEAM-DAG: seam.rule.row.mset-hit:
// SEAM-DAG: seam.rule.row.mcopy:
// SEAM-DAG: seam.rule.premise.a-inb:
// SEAM-DAG: seam.rule.memop.translated: 0
// DAG-NOT: vc.dag-op: m{{copy|set|havoc}}
// DAG: result: unsat
// SEAM: result: unsat
// ROW0: seam.rule.memop.translated: {{[1-9]}}
// ROW0: result: unsat
// LIMIT: rewrite.abort
// LIMIT: result: unsat
// CHECK1: result: unsat
// CHECK1: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0
// PLAIN: memset.symbolic-bound
// PLAIN: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @terms {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %c2 = sea.const 2 : i64
  %c8 = sea.const 8 : i64
  %ge = sea.icmp uge, %n, %c2 : i64
  sea.assume %ge
  %le = sea.icmp ule, %n, %c8 : i64
  sea.assume %le
  %k = sea.nondet "k" [4] : i64
  %c16 = sea.const 16 : i64
  %kle = sea.icmp ult, %k, %c16 : i64
  sea.assume %kle
  %pb = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %xb = sea.load %m0[%pb] : !m -> i8
  %c1 = sea.const 1 : i64
  %pa1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %pa16 = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64
  %z = sea.const 0 : i8
  // a[16 .. 16 + n) <- b[0 .. n); then a[0 .. 8) <- 0
  %m1 = sea.memmove %m0[%pa16], %m0[%b], %n inplace : !m
  %m2 = sea.memset %m1[%a], %z, %c8 inplace : !m
  %yb = sea.load %m2[%pb] : !m -> i8
  %ya = sea.load %m2[%pa1] : !m -> i8
  %pk = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %yk = sea.load %m2[%pk] : !m -> i8
  %e1 = sea.icmp eq, %yb, %xb : i8
  %e2 = sea.icmp eq, %ya, %z : i8
  %e12 = sea.and %e1, %e2 : i1
  // k < 8: a + k was set to 0
  %kin = sea.icmp uge, %k, %c8 : i64
  %e3 = sea.icmp eq, %yk, %z : i8
  %e4 = sea.or %kin, %e3 : i1
  %e = sea.and %e12, %e4 : i1
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
