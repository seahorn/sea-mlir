// Null-object acceptance, row 7 (docs/design/SeaM-null.md): more
// alternatives than the ite-leaf caps. q = ite(null, A, B, C, D) + k has five
// leaves (> maxPullLeaves: the sum is not pulled through the ite), so it has
// no pointer view and ARM decides the read of q over a store to E: the null
// provenance of q is vacuous for a read (size 0: dropped, cmp.size0-vacuous),
// the others are other objects than E (A-INB): arm.skip. The same
// conditional answer as the view case: the access condition of q is guarded
// (the null provenance false, the others live), no fallback. Unsat with and
// without --seam (k < 8).
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: not sea-run %s --input=7=1,12=3 --missing=zero | FileCheck %s --check-prefix=NULL
// RUN: sea-run %s --input=7=0,8=1,12=3 --missing=zero | FileCheck %s --check-prefix=OBJ
// PLAIN: result: unsat
// SEAM-NOT: vc.seam-inb-violated
// SEAM: seam.inb: assumed
// SEAM-DAG: seam.rule.arm.skip: {{[1-9]}}
// SEAM-DAG: seam.rule.cmp.size0-vacuous: {{[1-9]}}
// SEAM: result: unsat
// CHK-NOT: vc.seam-inb-violated
// CHK: result: unsat
// CHK: seam-check: firings={{[1-9][0-9]*}} {{.*}} invalid=0 unknown=0 unexpressible=0
// NULL: status: error
// NULL: message: memory access through the null pointer
// OBJ: status: halted
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @cap {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c16 align 8 [3] : i64 -> !sea.ptr<64>
  %D = sea.malloc %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %E = sea.malloc %c16 align 8 [5] : i64 -> !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  %b1 = sea.nondet "b1" [7] : i1
  %b2 = sea.nondet "b2" [8] : i1
  %b3 = sea.nondet "b3" [9] : i1
  %b4 = sea.nondet "b4" [10] : i1
  %p4 = sea.select %b4, %C, %D : !sea.ptr<64>
  %p3 = sea.select %b3, %B, %p4 : !sea.ptr<64>
  %p2 = sea.select %b2, %A, %p3 : !sea.ptr<64>
  %p = sea.select %b1, %null, %p2 : !sea.ptr<64>
  %k = sea.nondet "k" [12] : i64
  %c8 = sea.const 8 : i64
  %kok = sea.icmp ult, %k, %c8 : i64
  sea.assume %kok
  %q = sea.ptr.add %p, %k : !sea.ptr<64>, i64
  %y = sea.load %m0[%q] : !m -> i8
  %v5 = sea.const 5 : i8
  %m1 = sea.store %m0[%E], %v5 inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
