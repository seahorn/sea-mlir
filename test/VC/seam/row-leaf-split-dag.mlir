// R-LEAVES over an ite DAG, through a node, and partial (row.leaf-split).
// @dag: four field pointers stored into S, read at an address built by five
// selects on independent conditions: an ite with 13 tree leaves and 3
// distinct ones (S, S+8, S+24). The bound is on distinct leaves, so the
// read splits and the loaded pointer is the ite of the stored ones: no open
// alias question, no loaded pointer left.
// RUN: sea-vc %s --lower --program=dag --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,LEAVES
// RUN: sea-vc %s --lower --program=dag --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,LEAFITE
// @node: the read is at (that ite) + 8, a node over an ite base with more
// leaves than ptrindex.pull-ite pulls: it splits per leaf node.
// RUN: sea-vc %s --lower --program=node --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,NODE,LEAVES
// RUN: sea-vc %s --lower --program=node --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,NODE,LEAFITE
// @partial: one leaf is a pointer loaded from the initial memory (its read is
// undecided against every store). Default: no split. With
// --seam-leaf-split-partial the two stored leaves split and the undecided one
// stays one read (the residual); its open questions stay.
// RUN: sea-vc %s --lower --program=partial --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=NOPART
// RUN: sea-vc %s --lower --program=partial --seam --seam-leaf-split-partial --seam-stats 2>&1 | FileCheck %s --check-prefixes=PART,LEAVES
// RUN: sea-vc %s --lower --program=partial --seam --seam-wide --seam-leaf-split-partial --seam-stats 2>&1 | FileCheck %s --check-prefixes=PART,LEAFITE
// @partialsat: the same split; the assertion fails on the S + 8 leaf, and
// the counterexample replays.
// RUN: sea-vc %s --lower --program=partialsat --seam --seam-leaf-split-partial --seam-stats 2>&1 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=partialsat --seam --seam-wide --seam-leaf-split-partial --seam-stats 2>&1 | FileCheck %s --check-prefix=SAT
// @prune: leaves Q1, Q2 are loaded from the initial memory (undecided), S and
// S + 8 are stored; the residual is the ite pruned to its undecided leaves
// (16 residual leaves over the splits). @prunesat: the same, the assertion
// fails on the S leaf and the counterexample replays.
// RUN: sea-vc %s --lower --program=prune --seam --seam-leaf-split-partial --seam-stats 2>&1 | FileCheck %s --check-prefix=PRUNE
// RUN: %if yices %{ sea-vc %s --lower --program=prune --seam --seam-leaf-split-partial --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: sea-vc %s --lower --program=prunesat --seam --seam-leaf-split-partial --seam-stats 2>&1 | FileCheck %s --check-prefix=PRUNESAT
// Every firing validated (z3 with a short limit, unknown allowed; Yices and
// Bitwuzla decide them).
// RUN: sea-vc %s --lower --program=dag --seam --seam-check --seam-check-timeout=2 2>&1 | FileCheck %s --check-prefix=CHKZ
// RUN: %if yices %{ sea-vc %s --lower --program=node --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if yices %{ sea-vc %s --lower --program=node --seam --seam-wide --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if yices %{ sea-vc %s --lower --program=partial --seam --seam-leaf-split-partial --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if yices %{ sea-vc %s --lower --program=partial --seam --seam-wide --seam-leaf-split-partial --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if bitwuzla %{ sea-vc %s --lower --program=dag --seam --seam-wide --seam-check --solver=bitwuzla 2>&1 | FileCheck %s --check-prefix=CHK %}
// CHECK-NOT: seam.ptr.from-bits-left
// CHECK-NOT: seam.alias.open
// LEAVES: seam.rule.ptr.load-resolve-leaves: 1
// LEAFITE: seam.rule.ptr.load-resolve-leaf-ite: 1
// CHECK: seam.rule.row.leaf-split: {{[1-9]}}
// NODE: seam.rule.row.leaf-split-node: {{[1-9]}}
// CHECK: result: unsat
// NOPART-NOT: seam.rule.row.leaf-split:
// NOPART: result: unsat
// PART: seam.rule.row.leaf-split: {{[1-9]}}
// PART: seam.rule.row.leaf-split-partial: {{[1-9]}}
// PART: result: unsat
// PRUNE: seam.rule.row.leaf-split-residual-leaves: 16
// PRUNE: result: unsat
// PRUNESAT: result: sat
// PRUNESAT: validate: {{.*}}mismatches=0
// SAT: seam.rule.row.leaf-split-partial: {{[1-9]}}
// SAT: result: sat
// SAT: validate: {{.*}}mismatches=0
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// CHKZ: seam-check: {{.*}} valid={{[1-9][0-9]*}} invalid=0 unknown={{[0-9]+}} unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @dag {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %c24 = sea.const 24 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %P7 = sea.alloca %c8 align 8 [7] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %S24 = sea.ptr.add %S, %c24 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %m3 = sea.store %m2[%S24], %P7 inplace : !m, !sea.ptr<64>
  %c1 = sea.nondet "c1" [11] : i1
  %c2 = sea.nondet "c2" [12] : i1
  %c3 = sea.nondet "c3" [13] : i1
  %c4 = sea.nondet "c4" [14] : i1
  %c5 = sea.nondet "c5" [15] : i1
  %x1 = sea.select %c1, %S, %S8 : !sea.ptr<64>
  %x2 = sea.select %c2, %x1, %S24 : !sea.ptr<64>
  %x3 = sea.select %c3, %x2, %x1 : !sea.ptr<64>
  %x4 = sea.select %c4, %x3, %x2 : !sea.ptr<64>
  %x5 = sea.select %c5, %x4, %x3 : !sea.ptr<64>
  %q = sea.load %m3[%x5] : !m -> !sea.ptr<64>
  %v = sea.const 77 : i64
  %m5 = sea.store %m3[%q], %v inplace : !m, i64
  %r = sea.load %m5[%S] : !m -> !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %r, %P5 : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

sea.program @node {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %c24 = sea.const 24 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %P7 = sea.alloca %c8 align 8 [7] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %S24 = sea.ptr.add %S, %c24 : !sea.ptr<64>, i64
  %c16 = sea.const 16 : i64
  %c32 = sea.const 32 : i64
  %S16 = sea.ptr.add %S, %c16 : !sea.ptr<64>, i64
  %S32 = sea.ptr.add %S, %c32 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%S8], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S16], %P6 inplace : !m, !sea.ptr<64>
  %m3 = sea.store %m2[%S32], %P7 inplace : !m, !sea.ptr<64>
  %c1 = sea.nondet "c1" [11] : i1
  %c2 = sea.nondet "c2" [12] : i1
  %c3 = sea.nondet "c3" [13] : i1
  %c4 = sea.nondet "c4" [14] : i1
  %c5 = sea.nondet "c5" [15] : i1
  %x1 = sea.select %c1, %S, %S8 : !sea.ptr<64>
  %x2 = sea.select %c2, %x1, %S24 : !sea.ptr<64>
  %x3 = sea.select %c3, %x2, %x1 : !sea.ptr<64>
  %x4 = sea.select %c4, %x3, %x2 : !sea.ptr<64>
  %x5 = sea.select %c5, %x4, %x3 : !sea.ptr<64>
  %a = sea.ptr.add %x5, %c8 : !sea.ptr<64>, i64
  %q = sea.load %m3[%a] : !m -> !sea.ptr<64>
  %v = sea.const 77 : i64
  %m5 = sea.store %m3[%q], %v inplace : !m, i64
  %r = sea.load %m5[%S8] : !m -> !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %r, %P5 : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

sea.program @partial {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %c24 = sea.const 24 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %P7 = sea.alloca %c8 align 8 [7] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %S24 = sea.ptr.add %S, %c24 : !sea.ptr<64>, i64
  %G = sea.alloca %c8 align 8 [9] : i64 -> !sea.ptr<64>
  %Q = sea.load %m0[%G] : !m -> !sea.ptr<64>
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %c1 = sea.nondet "c1" [11] : i1
  %c2 = sea.nondet "c2" [12] : i1
  %y = sea.select %c2, %S8, %Q : !sea.ptr<64>
  %x = sea.select %c1, %S, %y : !sea.ptr<64>
  %q = sea.load %m2[%x] : !m -> !sea.ptr<64>
  %t = sea.const 1 : i1
  %eq = sea.ptr.cmp eq, %q, %P5 : !sea.ptr<64>
  %nc1 = sea.xor %c1, %t : i1
  %ok = sea.or %nc1, %eq : i1
  sea.assert %ok
  sea.halt
}

sea.program @partialsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %c24 = sea.const 24 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %P7 = sea.alloca %c8 align 8 [7] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %S24 = sea.ptr.add %S, %c24 : !sea.ptr<64>, i64
  %G = sea.alloca %c8 align 8 [9] : i64 -> !sea.ptr<64>
  %Q = sea.load %m0[%G] : !m -> !sea.ptr<64>
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %c1 = sea.nondet "c1" [11] : i1
  %c2 = sea.nondet "c2" [12] : i1
  %y = sea.select %c2, %S8, %Q : !sea.ptr<64>
  %x = sea.select %c1, %S, %y : !sea.ptr<64>
  %q = sea.load %m2[%x] : !m -> !sea.ptr<64>
  %t = sea.const 1 : i1
  %eq = sea.ptr.cmp eq, %q, %P5 : !sea.ptr<64>
  %live = sea.or %c1, %c2 : i1
  sea.assume %live
  sea.assert %eq
  sea.halt
}

sea.program @prune {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %G1 = sea.alloca %c8 align 8 [9] : i64 -> !sea.ptr<64>
  %G2 = sea.alloca %c8 align 8 [10] : i64 -> !sea.ptr<64>
  %Q1 = sea.load %m0[%G1] : !m -> !sea.ptr<64>
  %Q2 = sea.load %m0[%G2] : !m -> !sea.ptr<64>
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %c1 = sea.nondet "c1" [11] : i1
  %c2 = sea.nondet "c2" [12] : i1
  %c3 = sea.nondet "c3" [13] : i1
  %c4 = sea.nondet "c4" [14] : i1
  %a = sea.select %c3, %Q1, %S : !sea.ptr<64>
  %b0 = sea.select %c4, %Q2, %Q1 : !sea.ptr<64>
  %b = sea.select %c2, %S8, %b0 : !sea.ptr<64>
  %x = sea.select %c1, %a, %b : !sea.ptr<64>
  %q = sea.load %m2[%x] : !m -> !sea.ptr<64>
  %t = sea.const 1 : i1
  %e5 = sea.ptr.cmp eq, %q, %P5 : !sea.ptr<64>
  %nc3 = sea.xor %c3, %t : i1
  %g = sea.and %c1, %nc3 : i1
  %ng = sea.xor %g, %t : i1
  %ok = sea.or %ng, %e5 : i1
  sea.assert %ok
  sea.halt
}

sea.program @prunesat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %G1 = sea.alloca %c8 align 8 [9] : i64 -> !sea.ptr<64>
  %G2 = sea.alloca %c8 align 8 [10] : i64 -> !sea.ptr<64>
  %Q1 = sea.load %m0[%G1] : !m -> !sea.ptr<64>
  %Q2 = sea.load %m0[%G2] : !m -> !sea.ptr<64>
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %c1 = sea.nondet "c1" [11] : i1
  %c2 = sea.nondet "c2" [12] : i1
  %c3 = sea.nondet "c3" [13] : i1
  %c4 = sea.nondet "c4" [14] : i1
  %a = sea.select %c3, %Q1, %S : !sea.ptr<64>
  %b0 = sea.select %c4, %Q2, %Q1 : !sea.ptr<64>
  %b = sea.select %c2, %S8, %b0 : !sea.ptr<64>
  %x = sea.select %c1, %a, %b : !sea.ptr<64>
  %q = sea.load %m2[%x] : !m -> !sea.ptr<64>
  %t = sea.const 1 : i1
  %e5 = sea.ptr.cmp eq, %q, %P5 : !sea.ptr<64>
  %g = sea.and %c1, %c3 : i1
  sea.assume %g
  sea.assert %e5
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
