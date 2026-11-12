// A wide read over a store-map is decided per leaf of its address with the
// access context and stays a word: the ite of the leaf values. A leaf in
// another object than the map's is Out by A-INB (cmp.diff-base: the read
// moves below the map); a leaf at a numeral offset of the map's object reads
// the entries it covers and the word below for the rest (exact). It used to
// be decided only for a single pointer view into another object and split
// into byte reads otherwise (wide.split-read; at an ite address, byte reads
// of the ite).
// @cross: byte writes to D (a store-map), a read at ite(c, A + 8, B): both
// leaves skip the map.
// @same: byte writes at A + 8 and A + 9, a read at ite(c, A + 8, A + 16): the
// two entries over the old word on one leaf, the old word on the other.
// @single: the same writes, a read at A + 8 (no ite): one word, not split.
// RUN: sea-vc %s --lower --program=cross --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,CROSS
// RUN: sea-vc %s --lower --program=same --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,SAME
// RUN: sea-vc %s --lower --program=single --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,SINGLE
// RUN: sea-vc %s --lower --program=cross --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=same --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=single --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open
// CROSS: seam.rule.premise.a-inb: {{[1-9]}}
// SINGLE: seam.rule.wide.smap-hit: 1
// SAME: seam.rule.wide.smap-leaf-hit: 1
// SAME: seam.rule.wide.smap-leaf-miss: 1
// CROSS: seam.rule.wide.smap-leaf-skip: 2
// SAME: seam.rule.wide.smap-split: 1
// CROSS: seam.rule.wide.smap-split: 1
// CHECK-NOT: seam.rule.wide.split-read
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @cross {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %c1 = sea.const 1 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %D = sea.malloc %c32 align 8 [4] : i64 -> !sea.ptr<64>
  %A8 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %D1 = sea.ptr.add %D, %c1 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A8, %B : !sea.ptr<64>
  %r = sea.load %m0[%q] : !m -> i64
  %x = sea.const 0x11 : i8
  %y = sea.const 0x22 : i8
  %m1 = sea.store %m0[%D], %x inplace : !m, i8
  %m2 = sea.store %m1[%D1], %y inplace : !m, i8
  %l = sea.load %m2[%q] : !m -> i64
  %ok = sea.icmp eq, %l, %r : i64
  sea.assert %ok
  sea.halt
}
sea.program @same {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %c9 = sea.const 9 : i64
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %A8 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %A9 = sea.ptr.add %A, %c9 : !sea.ptr<64>, i64
  %A16 = sea.ptr.add %A, %c16 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A8, %A16 : !sea.ptr<64>
  %r8 = sea.load %m0[%A8] : !m -> i64
  %r16 = sea.load %m0[%A16] : !m -> i64
  %x = sea.const 0x11 : i8
  %y = sea.const 0x22 : i8
  %m1 = sea.store %m0[%A8], %x inplace : !m, i8
  %m2 = sea.store %m1[%A9], %y inplace : !m, i8
  %l = sea.load %m2[%q] : !m -> i64
  %hi = sea.const 0xffffffffffff0000 : i64
  %lo = sea.const 0x2211 : i64
  %h = sea.and %r8, %hi : i64
  %e8 = sea.or %h, %lo : i64
  %e = sea.select %c, %e8, %r16 : i64
  %ok = sea.icmp eq, %l, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @single {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c8 = sea.const 8 : i64
  %c9 = sea.const 9 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %A8 = sea.ptr.add %A, %c8 : !sea.ptr<64>, i64
  %A9 = sea.ptr.add %A, %c9 : !sea.ptr<64>, i64
  %r8 = sea.load %m0[%A8] : !m -> i64
  %x = sea.const 0x11 : i8
  %y = sea.const 0x22 : i8
  %m1 = sea.store %m0[%A8], %x inplace : !m, i8
  %m2 = sea.store %m1[%A9], %y inplace : !m, i8
  %l = sea.load %m2[%A8] : !m -> i64
  %hi = sea.const 0xffffffffffff0000 : i64
  %lo = sea.const 0x2211 : i64
  %h = sea.and %r8, %hi : i64
  %e = sea.or %h, %lo : i64
  %ok = sea.icmp eq, %l, %e : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
