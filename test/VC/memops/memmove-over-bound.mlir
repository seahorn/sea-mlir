// Without --seam, symbolic memmove lengths are bounded at 16, so executions
// with n > 16 are excluded. With --seam, the MCOPY term handles symbolic n
// exactly.
// A[4..4+n) <- A[0..n), n <= K nondet; the assert says cell 4+k is unchanged, which fails when n > k
// (cells distinct). The interpreter confirms assert-failed for n = k+1 in
// all three programs, so the correct verdict is sat:
//   @edge_16   sat    (n = 16 fits the bound)
//   @over_bound, @edge_17   UNSAT -- sea-expand-memset adds assume(n <= 16),
//   so the executions with n > 16 are cut (warning memset.symbolic-bound).
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: over_bound unsat warn:memset.symbolic-bound
// EXPECT-SEAM: over_bound sat
// EXPECT: edge_16 sat warn:memset.symbolic-bound
// EXPECT-SEAM: edge_16 sat
// EXPECT: edge_17 unsat warn:memset.symbolic-bound
// EXPECT-SEAM: edge_17 sat
// INTERP: over_bound assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=20
// INTERP: edge_16 assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=16
// INTERP: edge_17 assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=17
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @over_bound {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64_i64 = sea.const 64 : i64
  %A = sea.malloc %c64_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %c32_i64 = sea.const 32 : i64
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c40_i64 = sea.const 40 : i64
  %b2 = sea.icmp ule, %n1, %c40_i64 : i64
  sea.assume %b2
  %c23_i64 = sea.const 23 : i64
  %p3 = sea.ptr.add %A, %c23_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %c19_i64 = sea.const 19 : i64
  %p5 = sea.ptr.add %A, %c19_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m8 = sea.memmove %m0[%p7], %m0[%A], %n1 inplace : !m
  %p9 = sea.ptr.add %A, %c23_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m8[%p9] : !m -> i8
  %e11 = sea.icmp eq, %x10, %x4 : i8
  sea.assert %e11
  sea.halt
}
sea.program @edge_16 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64_i64 = sea.const 64 : i64
  %A = sea.malloc %c64_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %c32_i64 = sea.const 32 : i64
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c16_i64 = sea.const 16 : i64
  %b2 = sea.icmp ule, %n1, %c16_i64 : i64
  sea.assume %b2
  %c19_i64 = sea.const 19 : i64
  %p3 = sea.ptr.add %A, %c19_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %c15_i64 = sea.const 15 : i64
  %p5 = sea.ptr.add %A, %c15_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m8 = sea.memmove %m0[%p7], %m0[%A], %n1 inplace : !m
  %p9 = sea.ptr.add %A, %c19_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m8[%p9] : !m -> i8
  %e11 = sea.icmp eq, %x10, %x4 : i8
  sea.assert %e11
  sea.halt
}
sea.program @edge_17 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64_i64 = sea.const 64 : i64
  %A = sea.malloc %c64_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %c32_i64 = sea.const 32 : i64
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c17_i64 = sea.const 17 : i64
  %b2 = sea.icmp ule, %n1, %c17_i64 : i64
  sea.assume %b2
  %c20_i64 = sea.const 20 : i64
  %p3 = sea.ptr.add %A, %c20_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %c16_i64 = sea.const 16 : i64
  %p5 = sea.ptr.add %A, %c16_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m8 = sea.memmove %m0[%p7], %m0[%A], %n1 inplace : !m
  %p9 = sea.ptr.add %A, %c20_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m8[%p9] : !m -> i8
  %e11 = sea.icmp eq, %x10, %x4 : i8
  sea.assert %e11
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
