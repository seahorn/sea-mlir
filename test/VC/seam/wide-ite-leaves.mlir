// W-ITE: a wide read over a wide write, either address an ite, is decided
// per leaf pair and stays a word: the value is the ite of the leaf values
// (Hit: the stored value, Sub(k): its bytes, Skip: the read below), not the
// concat of byte reads over byte stores (wide.split-read,
// wide.expand-write), which loses the word shape of a loaded value.
// @mixed: 8-byte stores at A and A + 24, a read at ite(c, A + 24, A): Hit on
// one leaf and Skip on the other at each store: ite(c, 7, 5).
// @sub: an 8-byte store at ite(c, A, B), a 4-byte read at
// ite(c, A + 4, B + 4): Sub(4) on both leaves, the high half of the value.
// @cross: an 8-byte store at B, a read at ite(c, A + 8, A): Skip on both
// leaves by A-INB (no `align`: neither W-ROW-ALIGNED nor congruence).
// RUN: sea-vc %s --lower --program=mixed --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,MIXED
// RUN: sea-vc %s --lower --program=sub --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,SUB
// RUN: sea-vc %s --lower --program=cross --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,CROSS
// (z3 runs out of the 10 s check limit on the mixed firing, a z3
// performance case, not soundness; Bitwuzla and Yices also run it)
// RUN: %if bitwuzla %{ sea-vc %s --lower --program=mixed --seam --seam-wide --seam-check --solver=bitwuzla 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if yices %{ sea-vc %s --lower --program=mixed --seam --seam-wide --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: sea-vc %s --lower --program=sub --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=cross --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open
// CROSS: seam.rule.premise.a-inb: {{[1-9]}}
// CHECK-NOT: seam.rule.wide.expand-write
// MIXED: seam.rule.wide.ite-leaf-hit: 2
// MIXED: seam.rule.wide.ite-leaf-skip: 2
// SUB: seam.rule.wide.ite-leaf-sub: 2
// CROSS: seam.rule.wide.ite-leaf-skip: 2
// CHECK: seam.rule.wide.ite-split: {{[1-9]}}
// CHECK-NOT: seam.rule.wide.split-read
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mixed {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c24 = sea.const 24 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %A24 = sea.ptr.add %A, %c24 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A24, %A : !sea.ptr<64>
  %v5 = sea.const 5 : i64
  %v7 = sea.const 7 : i64
  %m1 = sea.store %m0[%A], %v5 inplace : !m, i64
  %m2 = sea.store %m1[%A24], %v7 inplace : !m, i64
  %x = sea.load %m2[%q] : !m -> i64
  %e = sea.select %c, %v7, %v5 : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @sub {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c4 = sea.const 4 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %A4 = sea.ptr.add %A, %c4 : !sea.ptr<64>, i64
  %B4 = sea.ptr.add %B, %c4 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %p = sea.select %c, %A, %B : !sea.ptr<64>
  %q = sea.select %c, %A4, %B4 : !sea.ptr<64>
  %v = sea.const 0x1122334455667788 : i64
  %m1 = sea.store %m0[%p], %v inplace : !m, i64
  %x = sea.load %m1[%q] : !m -> i32
  %e = sea.const 0x11223344 : i32
  %ok = sea.icmp eq, %x, %e : i32
  sea.assert %ok
  sea.halt
}
sea.program @cross {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %A8 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A8, %A : !sea.ptr<64>
  %r = sea.load %m0[%q] : !m -> i64
  %v = sea.const 7 : i64
  %m1 = sea.store %m0[%B], %v inplace : !m, i64
  %x = sea.load %m1[%q] : !m -> i64
  %ok = sea.icmp eq, %x, %r : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
