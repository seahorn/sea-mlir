// W-BYTE-IN at a write of the whole address space (w = 2^n): 2-bit
// addresses, a 4-byte store at a + k, a byte read at a + j (one object,
// symbolic offsets). The guard (j - i) <u w had the numeral w = 4 truncated to
// 0 at 2 bits: always false, the read went to the old memory: wrong unsat
// (@fc: plain sat; @all: every byte is the stored one, unsat). The
// access leaves its 3-byte object, so the run falls back to no A-INB; the rule is
// exact (no premise) and fires there.
// RUN: sea-vc %s --program=fc --seam --seam-wide --seam-check --seam-stats 2>&1 | FileCheck %s --check-prefix=FC
// RUN: sea-vc %s --program=fc --lower 2>&1 | FileCheck %s --check-prefix=FC-PLAIN
// RUN: sea-vc %s --program=all --seam --seam-wide --seam-check --seam-stats 2>&1 | FileCheck %s --check-prefix=ALL
// FC: seam.rule.wide.byte-in: 1
// FC: result: sat
// FC: replay: assert-failed
// FC: seam-check: {{.*}} invalid=0
// FC-PLAIN: result: sat
// ALL: seam.rule.wide.byte-in: 1
// ALL: result: unsat
// ALL: seam-check: {{.*}} invalid=0
!am = !sea.mem<!sea.addr<2>, i8>
sea.program @fc {
  %a = sea.segment "heap.1" base 0 : i2 size 3 : i2 align 1 kind "heap" : !sea.addr<2>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %k = sea.nondet "k" [1] : i2
  %j = sea.nondet "j" [2] : i2
  %p = sea.addr.add %a, %k : !sea.addr<2>, i2
  %q = sea.addr.add %a, %j : !sea.addr<2>, i2
  %v = sea.const -1 : i32
  %m1 = sea.store %m0[%p], %v inplace : !am, i32
  %x = sea.load %m1[%q] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @all {
  %a = sea.segment "heap.1" base 0 : i2 size 3 : i2 align 1 kind "heap" : !sea.addr<2>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %k = sea.nondet "k" [1] : i2
  %j = sea.nondet "j" [2] : i2
  %p = sea.addr.add %a, %k : !sea.addr<2>, i2
  %q = sea.addr.add %a, %j : !sea.addr<2>, i2
  %v = sea.const -1 : i32
  %m1 = sea.store %m0[%p], %v inplace : !am, i32
  %x = sea.load %m1[%q] : !am -> i8
  %ff = sea.const -1 : i8
  %ok = sea.icmp eq, %x, %ff : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
