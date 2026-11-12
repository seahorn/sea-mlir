// W-ROW-ALIGNED decides its compare as ROW does, with the access context:
// an 8-byte read at ite(c, A + 24, A) over an 8-byte store at B (all
// `align 8`) is a skip on both leaves by A-INB (cmp.diff-base). It used to
// emit IS_OVERLAP(B, 1, ite(...), 1), which only the program compare (no
// A-INB) saw: an open alias question in the query.
// RUN: sea-vc %s --lower --seam --seam-wide --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open
// CHECK: seam.rule.premise.a-inb: {{[1-9]}}
// CHECK: seam.rule.wide.row-aligned: 1
// CHECK: seam.rule.wide.skip: 1
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c24 = sea.const 24 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %A24 = sea.ptr.add %A, %c24 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A24, %A : !sea.ptr<64>
  %v = sea.const 7 : i64
  %r = sea.load %m0[%q] align 8 : !m -> i64
  %m1 = sea.store %m0[%B], %v inplace align 8 : !m, i64
  %x = sea.load %m1[%q] align 8 : !m -> i64
  %ok = sea.icmp eq, %x, %r : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
