// PtrSort D13 (S4): an alias question the rewriter leaves
// open is explicit in the formula: the compare of a ROW step SeaM cannot
// decide is IS_OVERLAP(p, 1, q, 1), counted by --seam-stats
// (seam.alias.open.<kind>, by what the views of the two pointers say) and
// erased to the address equality at the boundary. A program compare stays
// a pointer compare (PTR_EQ): it is the program's question, not an alias
// question. @loaded: a store through a loaded pointer (no provenance), a
// read of object B: one-unknown. @prog: only a program compare: none open.
// RUN: sea-vc %s --lower --program=loaded --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=LOADED
// RUN: sea-vc %s --lower --program=loaded --seam --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --program=loaded --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=prog --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=PROG
// LOADED: seam.alias.open.all: 1
// LOADED: seam.alias.open.one-unknown: 1
// LOADED: result: sat
// LOADED: validate: checked={{[0-9]+}} mismatches=0
// SMT-NOT: overlap
// SMT: (check-sat)
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// PROG-NOT: seam.alias.open
// PROG: result:
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @loaded {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %p = sea.load %m0[%A] : !m -> !sea.ptr<64>
  %y = sea.load %m0[%B] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %x = sea.load %m1[%B] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @prog {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %e = sea.ptr.cmp eq, %A, %B : !sea.ptr<64>
  %f = sea.const 0 : i1
  %ok = sea.icmp eq, %e, %f : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
