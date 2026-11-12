// W-ROW-ALIGNED decides its compare as ROW does, with the access context:
// an 8-byte read at B over an 8-byte store at ite(c, A, p), p a loaded
// pointer (all `align 8`). The pair (A, B) is apart by A-INB
// (cmp.diff-base); (p, B) stays open, and the read is not split into
// leaves (W-ITE needs every open pair to be at a leaf of the read), so the
// guard is ROW's compare tree: ite(c, false, IS_OVERLAP(p, 1, B, 1)). It
// used to be IS_OVERLAP(ite(c, A, p), 1, B, 1), decided later only as a
// program compare (no A-INB): the A leaf stayed in the alias question.
// RUN: sea-vc %s --lower --seam --seam-wide --seam-stats --vcgen-only 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.alias.open.all: 1
// CHECK: seam.alias.open.one-unknown: 1
// CHECK: seam.alias.open-pairs.all: 1
// CHECK: seam.rule.cmp.diff-base: {{[1-9]}}
// CHECK: seam.rule.premise.a-inb: {{[1-9]}}
// CHECK: seam.rule.wide.row-aligned: 1
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c32 align 8 [4] : i64 -> !sea.ptr<64>
  %p = sea.load %m0[%C] align 8 : !m -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A, %p : !sea.ptr<64>
  %v = sea.const 7 : i64
  %r = sea.load %m0[%B] align 8 : !m -> i64
  %m1 = sea.store %m0[%q], %v inplace align 8 : !m, i64
  %x = sea.load %m1[%B] align 8 : !m -> i64
  %ok = sea.icmp eq, %x, %r : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
