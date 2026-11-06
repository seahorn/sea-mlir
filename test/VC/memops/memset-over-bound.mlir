// Without --seam, sea-expand-memset bounds symbolic lengths at 16, so
// executions with n > 16 are excluded. With --seam, the MSET term handles
// symbolic n exactly.
// Cell k is assumed not 0xFF before; memset(A, 0xFF, n) with n <= K nondet; the assert says cell k is NOT 0xFF, which
// fails exactly when n > k. The interpreter confirms assert-failed for n =
// k+1 in all three programs, so the correct verdict is sat:
//   @edge_16   sat    (n = 16 fits the bound)
//   @over_bound, @edge_17   UNSAT -- WRONG w.r.t. the interpreter: sea-expand-memset adds assume(n <= 16),
//   so the executions with n > 16 are cut (warning memset.symbolic-bound).
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: over_bound unsat warn:memset.symbolic-bound
// EXPECT-SEAM: over_bound sat
// EXPECT: edge_16 sat warn:memset.symbolic-bound
// EXPECT-SEAM: edge_16 sat
// EXPECT: edge_17 unsat warn:memset.symbolic-bound
// EXPECT-SEAM: edge_17 sat
// INTERP: over_bound assert-failed 3=20
// INTERP: edge_16 assert-failed 3=16
// INTERP: edge_17 assert-failed 3=17
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @over_bound {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64_i64 = sea.const 64 : i64
  %A = sea.malloc %c64_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c40_i64 = sea.const 40 : i64
  %b2 = sea.icmp ule, %n1, %c40_i64 : i64
  sea.assume %b2
  %c255_i8 = sea.const 255 : i8
  %c19_i64 = sea.const 19 : i64
  %p3 = sea.ptr.add %A, %c19_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %e5 = sea.icmp ne, %x4, %c255_i8 : i8
  sea.assume %e5
  %m6 = sea.memset %m0[%A], %c255_i8, %n1 inplace : !m
  %p7 = sea.ptr.add %A, %c19_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m6[%p7] : !m -> i8
  %e9 = sea.icmp ne, %x8, %c255_i8 : i8
  sea.assert %e9
  sea.halt
}
sea.program @edge_16 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64_i64 = sea.const 64 : i64
  %A = sea.malloc %c64_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c16_i64 = sea.const 16 : i64
  %b2 = sea.icmp ule, %n1, %c16_i64 : i64
  sea.assume %b2
  %c255_i8 = sea.const 255 : i8
  %c15_i64 = sea.const 15 : i64
  %p3 = sea.ptr.add %A, %c15_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %e5 = sea.icmp ne, %x4, %c255_i8 : i8
  sea.assume %e5
  %m6 = sea.memset %m0[%A], %c255_i8, %n1 inplace : !m
  %p7 = sea.ptr.add %A, %c15_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m6[%p7] : !m -> i8
  %e9 = sea.icmp ne, %x8, %c255_i8 : i8
  sea.assert %e9
  sea.halt
}
sea.program @edge_17 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64_i64 = sea.const 64 : i64
  %A = sea.malloc %c64_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c17_i64 = sea.const 17 : i64
  %b2 = sea.icmp ule, %n1, %c17_i64 : i64
  sea.assume %b2
  %c255_i8 = sea.const 255 : i8
  %c16_i64 = sea.const 16 : i64
  %p3 = sea.ptr.add %A, %c16_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %e5 = sea.icmp ne, %x4, %c255_i8 : i8
  sea.assume %e5
  %m6 = sea.memset %m0[%A], %c255_i8, %n1 inplace : !m
  %p7 = sea.ptr.add %A, %c16_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m6[%p7] : !m -> i8
  %e9 = sea.icmp ne, %x8, %c255_i8 : i8
  sea.assert %e9
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
