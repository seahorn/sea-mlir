// Tracking on objects of size 0 (docs/design/SeaM-null.md, "Shadow
// memories").
// The abstract interpreter, the plain lowered VC and --seam (with replay)
// must agree. p = malloc(0) has its own base (sea-place-static reserves one
// unit), so its shadow cell is not q's; the shadow memory is read by object
// identity (shadow.distinct-base, premise LAYOUT), never by A-INB
// (no cmp.size0-vacuous, no fallback) on its accesses.
// @track: reset through select(c, p, a) does not reset q: assert fails.
// @null: reset of null does not reset q: assert fails.
// @self: is_modified(p) after a tracked store to q is false: holds.
// Every sea-vc run must exit 0 (sat: replay agrees with the model).
// RUN: sea-run %s --program=track --input=4=1 --missing=zero | FileCheck %s --check-prefix=FAIL
// RUN: sea-vc %s --program=track --lower --solver=z3 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=track --lower --seam --seam-stats --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=SAT,SEAM,CHK
// RUN: %if yices %{ sea-vc %s --program=track --lower --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefixes=SAT,CHK %}
// RUN: sea-run %s --program=null --missing=zero | FileCheck %s --check-prefix=FAIL
// RUN: sea-vc %s --program=null --lower --solver=z3 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=null --lower --seam --seam-stats --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=SAT,SEAM,CHK
// RUN: sea-run %s --program=self --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-vc %s --program=self --lower --solver=z3 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=self --lower --seam --seam-stats --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefixes=UNSAT,SEAM,CHK
// RUN: sea-opt %s --sea-apply-tracking --sea-lower-tracking | FileCheck %s --check-prefix=LOW
// SEAM-NOT: vc.seam-inb-violated
// SEAM: seam.inb: assumed
// SEAM-NOT: cmp.size0-vacuous
// SEAM: seam.rule.shadow.access: {{[1-9]}}
// SEAM: seam.rule.shadow.distinct-base: {{[1-9]}}
// FAIL: status: assert-failed
// OK: status: halted
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
// UNSAT: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// LOW: sea.mem.const %{{.*}} shadow : !sea.mem<!sea.ptr<64>, i8>
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @track {
  %m0 = sea.mem.nondet "m" [0] : !m
  %zero = sea.const 0 : i64
  %four = sea.const 4 : i64
  %p = sea.malloc %zero align 4 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %four align 4 [2] : i64 -> !sea.ptr<64>
  %a = sea.malloc %four align 4 [3] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [4] : i1
  sea.assume %c
  %s = sea.select %c, %p, %a : !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%q], %v inplace tracked : !m, i8
  %m2 = sea.mem.reset_modified %m1[%s] inplace : !m
  %mod = sea.mem.is_modified %m2[%q] : !m
  %false = sea.const 0 : i1
  %ok = sea.icmp eq, %mod, %false : i1
  sea.assert %ok
  sea.halt
}
sea.program @null {
  %m0 = sea.mem.nondet "m" [0] : !m
  %four = sea.const 4 : i64
  %q = sea.malloc %four align 4 [2] : i64 -> !sea.ptr<64>
  %n = sea.ptr.null : !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%q], %v inplace tracked : !m, i8
  %m2 = sea.mem.reset_modified %m1[%n] inplace : !m
  %mod = sea.mem.is_modified %m2[%q] : !m
  %false = sea.const 0 : i1
  %ok = sea.icmp eq, %mod, %false : i1
  sea.assert %ok
  sea.halt
}
sea.program @self {
  %m0 = sea.mem.nondet "m" [0] : !m
  %zero = sea.const 0 : i64
  %four = sea.const 4 : i64
  %p = sea.malloc %zero align 4 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %four align 4 [2] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%q], %v inplace tracked : !m, i8
  %mod = sea.mem.is_modified %m1[%p] : !m
  %false = sea.const 0 : i1
  %ok = sea.icmp eq, %mod, %false : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
