// ptr.load-resolve-concat.unshared: a split pointer load whose bytes have
// no common guard. The two halves of X's bits are stored as i32 at P + 8i
// (bytes 0-3) and P + 16k + 4 (bytes 4-7), P a loaded address, and X is
// loaded back at P + 8j in byte mode. --seam-congruence decides the byte
// pairs of different positions apart, and ptr.overlap.cancel-num makes the
// guard of the low bytes one term g1 (overlap with P + 8i) and that of the
// high bytes another term g2 (overlap with P + 16k): no condition is
// shared by all eight bytes. The rule splits on g2 (unshared); in its then
// side the high bytes are bits of X and the low bytes share g1, so a second
// split reassembles X: two pulls (a budget of 1 resolves nothing). A rule
// that splits only on shared conditions leaves the load a concat.
// RUN: sea-vc %s --lower --seam --seam-congruence --seam-pull-concat=8 --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-congruence --seam-pull-concat=1 --seam-stats 2>&1 | FileCheck %s --check-prefix=ONE
// RUN: sea-vc %s --lower --seam --seam-congruence --seam-pull-concat=8 --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.rule.ptr.load-resolve-concat: 1
// CHECK: seam.rule.ptr.load-resolve-concat.pulls: 2
// CHECK: seam.rule.ptr.load-resolve-concat.unshared: 1
// CHECK: result: unsat
// ONE-NOT: seam.rule.ptr.load-resolve-concat:
// ONE: seam.rule.ptr.load-resolve-concat.unshared: 1
// ONE: result: unsat
// CHK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.addr<64>, i8>
sea.program @halves {
  %H = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %X = sea.segment "heap.2" base 4112 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [0] : !m
  %P = sea.load %m0[%H] : !m -> !sea.addr<64>
  %i = sea.nondet "i" [1] : i64
  %j = sea.nondet "j" [2] : i64
  %k = sea.nondet "k" [3] : i64
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %c4 = sea.const 4 : i64
  %o1 = sea.mul %i, %c8 : i64
  %o2a = sea.mul %k, %c16 : i64
  %o2 = sea.add %o2a, %c4 : i64
  %oj = sea.mul %j, %c8 : i64
  %a1 = sea.addr.add %P, %o1 : !sea.addr<64>, i64
  %a2 = sea.addr.add %P, %o2 : !sea.addr<64>, i64
  %aj = sea.addr.add %P, %oj : !sea.addr<64>, i64
  %bits = sea.addr.to_bits %X : !sea.addr<64> to i64
  %lo = sea.extract %bits[0] : i64 -> i32
  %hi = sea.extract %bits[32] : i64 -> i32
  %m1 = sea.store %m0[%a1], %lo inplace : !m, i32
  %m2 = sea.store %m1[%a2], %hi inplace : !m, i32
  %r = sea.load %m2[%aj] : !m -> !sea.addr<64>
  %eq = sea.addr.cmp eq, %r, %X : !sea.addr<64>
  // i = j and 2k = j: both halves were stored at the load, which is X
  %ij = sea.icmp eq, %i, %j : i64
  %k2 = sea.add %k, %k : i64
  %kj = sea.icmp eq, %k2, %j : i64
  %g = sea.and %ij, %kj : i1
  %t1 = sea.const 1 : i1
  %ng = sea.xor %g, %t1 : i1
  %ok = sea.or %ng, %eq : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
