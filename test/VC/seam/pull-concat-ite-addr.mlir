// Byte mode (no --seam-wide): a pointer X stored through an ite address,
// ite(c, p + 8i, p + 8k), and loaded back at p + 8j, p a loaded pointer
// (no object, no view). Byte k of the load is ite(g_k, byte_k(X), old_k),
// g_k the overlap of byte k of the store with byte k of the load; the
// byte pairs k != k' are decided apart by --seam-congruence (8x + d, d not
// 0 mod 8). ptr.eq.cancel-num makes g_k one term g for every k (the common
// K cancels inside the ite), so concat-pull splits the eight bytes once on
// g and the then side reassembles X: ite(g, X, from_bits(old)). Without
// the cancellation the eight guards differ and the rule spends its 8
// pulls without a shared split. Split pointers left: the load of p itself
// (from nondet memory) and the old side (9 without the cancellation).
// RUN: sea-vc %s --lower --seam --seam-congruence --seam-pull-concat=8 --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-congruence --seam-pull-concat=8 --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.ptr.from-bits-left.concat: 2
// CHECK: seam.rule.ptr.load-resolve-concat: 1
// CHECK: seam.rule.ptr.load-resolve-concat.pulls: 1
// CHECK: result: unsat
// CHK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ite_addr {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %H = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %X = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %P = sea.load %m0[%H] : !m -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64
  %j = sea.nondet "j" [4] : i64
  %k = sea.nondet "k" [5] : i64
  %c = sea.nondet "c" [6] : i1
  %pi = sea.ptr.index %P [%i] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %pk = sea.ptr.index %P [%k] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %a = sea.select %c, %pi, %pk : !sea.ptr<64>
  %m1 = sea.store %m0[%a], %X inplace : !m, !sea.ptr<64>
  %pj = sea.ptr.index %P [%j] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %q = sea.load %m1[%pj] : !m -> !sea.ptr<64>
  %eq = sea.ptr.cmp eq, %q, %X : !sea.ptr<64>
  %ij = sea.icmp eq, %i, %j : i64
  %g = sea.and %c, %ij : i1
  %t1 = sea.const 1 : i1
  %ng = sea.xor %g, %t1 : i1
  %ok = sea.or %ng, %eq : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
