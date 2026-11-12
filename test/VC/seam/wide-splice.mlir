// W-SPLICE: a wide read over a narrower write inside it (a byte store under
// an 8-byte load) stays a word: the read below with the written bytes in
// place (concat of extracts of the word below and the stored value), per
// leaf pair when an address is an ite (W-ITE). It used to be undecided
// (wideRelation knew only Hit / Sub / Skip) and split into byte reads
// (wide.split-read; at an ite address, byte reads of the ite).
// @single: a byte store at A + 3, a read at A.
// @ite: a byte store at ite(c, A + 3, B + 3), a read at ite(c, A, B): byte 3
// of the read on both leaf pairs, one value.
// @mixed: a byte store at A + 3, a read at ite(c, A, B): the splice on the
// A leaf, the read below on the B leaf (A-INB).
// RUN: sea-vc %s --lower --program=single --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,SINGLE
// RUN: sea-vc %s --lower --program=ite --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,ITE
// RUN: sea-vc %s --lower --program=mixed --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,MIXED
// RUN: sea-vc %s --lower --program=single --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=ite --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --program=mixed --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open
// MIXED: seam.rule.premise.a-inb: {{[1-9]}}
// ITE: seam.rule.wide.ite-leaf-splice: 2
// MIXED: seam.rule.wide.ite-leaf-skip: 1
// MIXED: seam.rule.wide.ite-leaf-splice: 1
// ITE: seam.rule.wide.ite-split: 1
// MIXED: seam.rule.wide.ite-split: 1
// SINGLE: seam.rule.wide.splice: 1
// CHECK-NOT: seam.rule.wide.split-read
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @single {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c3 = sea.const 3 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %A3 = sea.ptr.add %A, %c3 : !sea.ptr<64>, i64
  %r = sea.load %m0[%A] : !m -> i64
  %v = sea.const 0x5a : i8
  %m1 = sea.store %m0[%A3], %v inplace : !m, i8
  %l = sea.load %m1[%A] : !m -> i64
  %hi = sea.const 0xffffffff00ffffff : i64
  %b = sea.const 0x5a000000 : i64
  %h = sea.and %r, %hi : i64
  %e = sea.or %h, %b : i64
  %ok = sea.icmp eq, %l, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @ite {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c3 = sea.const 3 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %A3 = sea.ptr.add %A, %c3 : !sea.ptr<64>, i64
  %B3 = sea.ptr.add %B, %c3 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %p = sea.select %c, %A3, %B3 : !sea.ptr<64>
  %q = sea.select %c, %A, %B : !sea.ptr<64>
  %r = sea.load %m0[%q] : !m -> i64
  %v = sea.const 0x5a : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %l = sea.load %m1[%q] : !m -> i64
  %hi = sea.const 0xffffffff00ffffff : i64
  %b = sea.const 0x5a000000 : i64
  %h = sea.and %r, %hi : i64
  %e = sea.or %h, %b : i64
  %ok = sea.icmp eq, %l, %e : i64
  sea.assert %ok
  sea.halt
}
sea.program @mixed {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c3 = sea.const 3 : i64
  %A = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %A3 = sea.ptr.add %A, %c3 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [3] : i1
  %q = sea.select %c, %A, %B : !sea.ptr<64>
  %r = sea.load %m0[%q] : !m -> i64
  %v = sea.const 0x5a : i8
  %m1 = sea.store %m0[%A3], %v inplace : !m, i8
  %l = sea.load %m1[%q] : !m -> i64
  %hi = sea.const 0xffffffff00ffffff : i64
  %b = sea.const 0x5a000000 : i64
  %h = sea.and %r, %hi : i64
  %eA = sea.or %h, %b : i64
  %e = sea.select %c, %eA, %r : i64
  %ok = sea.icmp eq, %l, %e : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
