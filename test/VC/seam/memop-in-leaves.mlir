// The in-test of a read over a memory-operation term is built over the
// leaf pairs (as ROW's compare tree): a byte read at ite(c, A, B + k) over
// an operation on [B, B + n): the A leaf is out by A-INB (cmp.diff-base),
// only the B leaf stays (k <u n). It used to stay one in-test of the whole
// ite, which the program rules (no A-INB) could not split.
// RUN: sea-vc %s --lower --seam --program=mset --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,MSET
// RUN: sea-vc %s --lower --seam --program=mcopy --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,MCOPY
// RUN: sea-vc %s --lower --seam --program=mhavoc --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,MHAVOC
// RUN: sea-vc %s --lower --seam --program=mset --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --seam --program=mcopy --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --seam --program=mhavoc --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.rule.cmp.diff-base: {{[1-9]}}
// CHECK: seam.rule.mem.in-push: 1
// CHECK: seam.rule.premise.a-inb: {{[1-9]}}
// MSET: seam.rule.row.mset: 1
// MCOPY: seam.rule.row.mcopy: 1
// MHAVOC: seam.rule.row.mhavoc: 1
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mset {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [3] : i64
  %n = sea.nondet "n" [4] : i64
  %c = sea.nondet "c" [5] : i1
  %Bk = sea.ptr.add %B, %k : !sea.ptr<64>, i64
  %q = sea.select %c, %A, %Bk : !sea.ptr<64>
  %r = sea.load %m0[%A] : !m -> i8
  %z = sea.const 0 : i8
  %m1 = sea.memset %m0[%B], %z, %n inplace : !m
  %l = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %l, %r : i8
  %t = sea.const 1 : i1
  %ok2 = sea.select %c, %ok, %t : i1
  sea.assert %ok2
  sea.halt
}
sea.program @mcopy {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c32 align 8 [6] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [3] : i64
  %n = sea.nondet "n" [4] : i64
  %c = sea.nondet "c" [5] : i1
  %Bk = sea.ptr.add %B, %k : !sea.ptr<64>, i64
  %q = sea.select %c, %A, %Bk : !sea.ptr<64>
  %r = sea.load %m0[%A] : !m -> i8
  %m1 = sea.memmove %m0[%B], %m0[%C], %n inplace : !m
  %l = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %l, %r : i8
  %t = sea.const 1 : i1
  %ok2 = sea.select %c, %ok, %t : i1
  sea.assert %ok2
  sea.halt
}
sea.program @mhavoc {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [3] : i64
  %n = sea.nondet "n" [4] : i64
  %c = sea.nondet "c" [5] : i1
  %Bk = sea.ptr.add %B, %k : !sea.ptr<64>, i64
  %q = sea.select %c, %A, %Bk : !sea.ptr<64>
  %r = sea.load %m0[%A] : !m -> i8
  %z = sea.const 0 : i8
  %mb = sea.store %m0[%B], %z inplace : !m, i8
  %m1 = sea.memhavoc %mb[%B], %n [7] inplace : !m
  %l = sea.load %m1[%q] : !m -> i8
  %r2 = sea.load %mb[%A] : !m -> i8
  %ok = sea.icmp eq, %l, %r2 : i8
  %t = sea.const 1 : i1
  %ok2 = sea.select %c, %ok, %t : i1
  sea.assert %ok2
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
