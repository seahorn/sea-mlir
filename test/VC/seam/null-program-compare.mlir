// Null-object acceptance, rows 5 and 6 (docs/design/SeaM-null.md, point 4):
// program compares (pointer equality) are never decided by base, by size 0
// or by the layout.
// @offset: null + k == A + j with symbolic k, j can hold (offsetof-style
// arithmetic on null reaches any address): sat with and without --seam, no
// cmp.diff-base / cmp.size0-vacuous.
// @layout: p == null for p = ite(c, A, B) is false only by the layout facts
// obj.k = <base>, which the solver has (the rewriter's constants are
// independent without them): the compare stays in the query (the --smt2 dump
// names obj.null), unsat by the solver.
// RUN: sea-vc %s --lower --program=offset | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=offset --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=SAT,NODEC
// RUN: sea-vc %s --lower --program=offset --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=SAT,CHK
// RUN: %if yices %{ sea-vc %s --lower --program=offset --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=SAT,CHK %}
// RUN: sea-vc %s --lower --program=layout | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=layout --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,NODEC
// RUN: sea-vc %s --lower --program=layout --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: %if yices %{ sea-vc %s --lower --program=layout --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK %}
// RUN: sea-vc %s --lower --program=layout --seam --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// RUN: sea-run %s --program=layout --input=2=1 | FileCheck %s --check-prefix=INTERP
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
// UNSAT: result: unsat
// NODEC-NOT: seam.rule.cmp.diff-base
// NODEC-NOT: seam.rule.cmp.size0-vacuous
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// SMT: (= obj.null {{.*}}obj.heap.1
// INTERP: status: halted
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @offset {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %null = sea.ptr.null : !sea.ptr<64>
  %p = sea.ptr.add %null, %k : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %e = sea.ptr.cmp eq, %p, %q : !sea.ptr<64>
  %t = sea.const 1 : i1
  %ok = sea.xor %e, %t : i1
  sea.assert %ok
  sea.halt
}
sea.program @layout {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [3] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [2] : i1
  %p = sea.select %c, %A, %B : !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  %e = sea.ptr.cmp eq, %p, %null : !sea.ptr<64>
  %t = sea.const 1 : i1
  %ok = sea.xor %e, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
