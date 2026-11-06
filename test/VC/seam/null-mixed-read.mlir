// Null-object acceptance, row 1 (docs/design/SeaM-null.md, "Assurance"): a
// read through p = ite(c, null, A + i) over a store to B. The null leaf is
// impossible for a read (size 0), the A leaf is in another object than B:
// every leaf pair is decided (cmp.size0-vacuous, cmp.diff-base) and the read
// skips the store, under the satisfiable full A-INB premise
// ite(c, false, i <= 15). The access is not a definite violation (guarded
// aggregation): no fallback, A-INB stays on. Unsat (plain as well: i < 16
// keeps A + i in A, and the address 0 is in no object).
// With addr.prune-size0 (docs/design/SeaM-provenance.md, default
// on) the null leaf is dropped from the access address first, under the same
// premise, and cmp.diff-base decides the one pair left; the compare path
// (cmp.size0-vacuous) is pinned with --seam-prune-size0=0.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --seam --seam-prune-size0=0 --seam-stats 2>&1 | FileCheck %s --check-prefix=NOPRUNE
// RUN: sea-vc %s --lower --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --seam --seam-prune-size0=0 --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: %if yices %{ sea-vc %s --lower --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: not sea-run %s --input=3=1 --missing=zero | FileCheck %s --check-prefix=NULL
// RUN: sea-run %s --input=3=0,4=3 --missing=zero | FileCheck %s --check-prefix=OBJ
// PLAIN: result: unsat
// SEAM-NOT: vc.seam-inb-violated
// SEAM: seam.inb: assumed
// SEAM-DAG: seam.rule.addr.prune-size0: {{[1-9]}}
// SEAM-DAG: seam.rule.cmp.diff-base: {{[1-9]}}
// SEAM-DAG: seam.rule.row.store-skip: {{[1-9]}}
// SEAM: result: unsat
// NOPRUNE-NOT: seam.rule.addr.prune-size0
// NOPRUNE-DAG: seam.rule.cmp.size0-vacuous: {{[1-9]}}
// NOPRUNE-DAG: seam.rule.cmp.diff-base: {{[1-9]}}
// NOPRUNE-DAG: seam.rule.row.store-skip: {{[1-9]}}
// NOPRUNE: result: unsat
// CHK-NOT: vc.seam-inb-violated
// CHK: result: unsat
// CHK: seam-check: firings={{[1-9][0-9]*}} {{.*}} invalid=0 unknown=0 unexpressible=0
// NULL: status: error
// NULL: message: memory access through the null pointer
// OBJ: status: halted
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mixed {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %i = sea.nondet "i" [4] : i64
  %iok = sea.icmp ult, %i, %c16 : i64
  sea.assume %iok
  %Ai = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %null = sea.ptr.null : !sea.ptr<64>
  %p = sea.select %c, %null, %Ai : !sea.ptr<64>
  %v5 = sea.const 5 : i8
  %m1 = sea.store %m0[%B], %v5 inplace : !m, i8
  %x = sea.load %m1[%p] : !m -> i8
  %y = sea.load %m0[%p] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
