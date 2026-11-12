// ROW's compare tree covers every leaf pair the access decision looks at
// (maxDecideLeaves): a byte read at a 5-leaf ite (4 leaves in A, 1 in B)
// over a store at B + j. The A leaves are false by A-INB (cmp.diff-base);
// only the B leaf is an alias question. With the tree capped at
// maxEqLeaves = 4 the whole ite stayed one IS_OVERLAP (one-unknown).
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open.one-unknown
// CHECK: seam.rule.premise.a-inb: {{[1-9]}}
// CHECK: result: sat
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %c24 = sea.const 24 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [3] : i64
  %j = sea.nondet "j" [4] : i64
  %c1 = sea.nondet "c1" [5] : i1
  %c2 = sea.nondet "c2" [6] : i1
  %c3 = sea.nondet "c3" [7] : i1
  %c4 = sea.nondet "c4" [8] : i1
  %A8 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %A16 = sea.ptr.add %A, %c16 : !sea.ptr<64>, i64
  %A24 = sea.ptr.add %A, %c24 : !sea.ptr<64>, i64
  %Bk = sea.ptr.add %B, %k : !sea.ptr<64>, i64
  %Bj = sea.ptr.add %B, %j : !sea.ptr<64>, i64
  %q4 = sea.select %c4, %A24, %Bk : !sea.ptr<64>
  %q3 = sea.select %c3, %A16, %q4 : !sea.ptr<64>
  %q2 = sea.select %c2, %A8, %q3 : !sea.ptr<64>
  %q = sea.select %c1, %A, %q2 : !sea.ptr<64>
  %r = sea.load %m0[%q] : !m -> i8
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%Bj], %z inplace : !m, i8
  %l = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %l, %r : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
