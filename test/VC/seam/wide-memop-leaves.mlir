// A wide read over a memory-operation term (mset / mcopy / mhavoc) at an
// ite address is decided per leaf pair with the access context (the in-test
// of the read span, decideInPair: cmp.diff-base under A-INB, mem.in-exact)
// and stays a word: the ite of the leaf values (In: the hit form, Out: the
// read below the term). It used to be decided only for single pointer views
// and split into byte reads at the ite address (wide.split-read).
// @mset: memset(D + 16, 0, 16), a read at ite(c, D + 16, A + 8): In on the D
// leaf (zero), Out on the A leaf (another object): ite(c, 0, <old word>).
// @mcopy / @mhavoc: a symbolic length at D, a read at ite(c, A, B): Out on
// both leaves by A-INB; the read moves below the term.
// RUN: sea-vc %s --lower --program=mset --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,MSET
// RUN: sea-vc %s --lower --program=mcopy --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,INB
// RUN: sea-vc %s --lower --program=mhavoc --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,INB
// RUN: sea-vc %s --lower --program=mset --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=mcopy --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=mhavoc --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open
// INB: seam.rule.premise.a-inb: {{[1-9]}}
// MSET: seam.rule.wide.memop-leaf-hit: 1
// CHECK: seam.rule.wide.memop-leaf-skip: {{[12]}}
// CHECK: seam.rule.wide.memop-split: 1
// CHECK-NOT: seam.rule.wide.split-read
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mset {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %D = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %A8 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %D16 = sea.ptr.add %D, %c16 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %D16, %A8 : !sea.ptr<64>
  %r = sea.load %m0[%A8] align 8 : !m -> i64
  %z = sea.const 0 : i8
  %m1 = sea.memset %m0[%D16], %z, %c16 inplace : !m
  %l = sea.load %m1[%q] align 8 : !m -> i64
  %z64 = sea.const 0 : i64
  %e = sea.select %c, %z64, %r : i64
  %ok = sea.icmp eq, %l, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @mcopy {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c32 align 8 [4] : i64 -> !sea.ptr<64>
  %D = sea.malloc %c32 align 8 [5] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [6] : i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A, %B : !sea.ptr<64>
  %r = sea.load %m0[%q] align 8 : !m -> i64
  %m1 = sea.memmove %m0[%D], %m0[%C], %n inplace : !m
  %l = sea.load %m1[%q] align 8 : !m -> i64
  %ok = sea.icmp eq, %l, %r : i64
  sea.assert %ok
  sea.halt
}
sea.program @mhavoc {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %D = sea.malloc %c32 align 8 [5] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [6] : i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A, %B : !sea.ptr<64>
  %r = sea.load %m0[%q] align 8 : !m -> i64
  %z = sea.const 0 : i8
  %md = sea.store %m0[%D], %z inplace : !m, i8
  %m1 = sea.memhavoc %md[%D], %n [7] inplace : !m
  %l = sea.load %m1[%q] align 8 : !m -> i64
  %ok = sea.icmp eq, %l, %r : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
