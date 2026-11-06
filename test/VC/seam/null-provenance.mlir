// Null-object acceptance, row 8 (docs/design/SeaM-null.md, point 7): null
// provenance comes only from sea.ptr.null and survives the operations that
// keep a pointer view. @stored: p = ite(c, null, A) is stored to memory and
// loaded back; the load resolves to p (row.store-hit), so the read through it
// over a store to B is decided per leaf (cmp.size0-vacuous, cmp.diff-base).
// @intzero: the same with the integer 0 cast to a pointer (sea.ptr.from_int
// 0): placement keeps it a number, not the null object, so nothing is vacuous
// (no cmp.size0-vacuous) and the solver decides 0 != B by the layout. Unsat
// everywhere; the interpreter reads both as null.
// With addr.prune-size0 (docs/design/SeaM-provenance.md, default
// on) @stored drops the null leaf of the access address first; @intzero's
// numeral leaf is kept (a numeral may be a valid address). The per-leaf
// compare path is pinned with --seam-prune-size0=0.
// RUN: sea-vc %s --lower --program=stored | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=stored --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=STORED
// RUN: sea-vc %s --lower --program=stored --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: %if yices %{ sea-vc %s --lower --program=stored --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK %}
// RUN: sea-vc %s --lower --program=stored --seam --seam-prune-size0=0 --seam-stats 2>&1 | FileCheck %s --check-prefix=NOPRUNE
// RUN: sea-vc %s --lower --program=stored --seam --seam-prune-size0=0 --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: not sea-run %s --program=stored --input=3=1 --missing=zero | FileCheck %s --check-prefix=NULL
// RUN: sea-vc %s --lower --program=intzero | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=intzero --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=INT
// RUN: sea-vc %s --lower --program=intzero --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: %if yices %{ sea-vc %s --lower --program=intzero --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK %}
// RUN: not sea-run %s --program=intzero --input=3=1 --missing=zero | FileCheck %s --check-prefix=NULL
// UNSAT: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// STORED-NOT: vc.seam-inb-violated
// STORED: seam.inb: assumed
// STORED-DAG: seam.rule.addr.prune-size0: {{[1-9]}}
// STORED-DAG: seam.rule.cmp.diff-base: {{[1-9]}}
// STORED: result: unsat
// NOPRUNE-NOT: seam.rule.addr.prune-size0
// NOPRUNE-DAG: seam.rule.cmp.size0-vacuous: {{[1-9]}}
// NOPRUNE-DAG: seam.rule.cmp.diff-base: {{[1-9]}}
// NOPRUNE: result: unsat
// INT-NOT: seam.rule.addr.prune-size0
// INT-NOT: seam.rule.cmp.size0-vacuous
// INT: result: unsat
// NULL: status: error
// NULL: message: memory access through the null pointer
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @stored {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %cell = sea.malloc %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %null = sea.ptr.null : !sea.ptr<64>
  %s = sea.select %c, %null, %A : !sea.ptr<64>
  %m1 = sea.store %m0[%cell], %s inplace : !m, !sea.ptr<64>
  %q = sea.load %m1[%cell] : !m -> !sea.ptr<64>
  %y = sea.load %m1[%q] : !m -> i8
  %v5 = sea.const 5 : i8
  %m2 = sea.store %m1[%B], %v5 inplace : !m, i8
  %x = sea.load %m2[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @intzero {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %cell = sea.malloc %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %z = sea.const 0 : i64
  %zp = sea.ptr.from_int %z : i64 to !sea.ptr<64>
  %s = sea.select %c, %zp, %A : !sea.ptr<64>
  %m1 = sea.store %m0[%cell], %s inplace : !m, !sea.ptr<64>
  %q = sea.load %m1[%cell] : !m -> !sea.ptr<64>
  %y = sea.load %m1[%q] : !m -> i8
  %v5 = sea.const 5 : i8
  %m2 = sea.store %m1[%B], %v5 inplace : !m, i8
  %x = sea.load %m2[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
