// memmove with a symbolic length between objects.
// n is a nondet bounded by assume(n <= 8), inside the expansion bound. A[6..6+n) <- B[2..2+n).
// @unsat: cell d-1 untouched, cell d+n untouched, first and last (n>0) cells copied, n=0 is a no-op.
// @sat_*: one claim off by one (see the program). The interpreter runs use distinct cell values
// (A: 10+i, B: 100+i) and n in 0, 1, 5, 8 for @unsat.
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: unsat unsat warn:memset.symbolic-bound
// EXPECT-SEAM: unsat unsat
// EXPECT: sat_last sat warn:memset.symbolic-bound
// EXPECT-SEAM: sat_last sat
// EXPECT: sat_hi sat warn:memset.symbolic-bound
// EXPECT-SEAM: sat_hi sat
// EXPECT: sat_zero sat warn:memset.symbolic-bound
// EXPECT-SEAM: sat_zero sat
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=0
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=1
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=5
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=8
// INTERP: sat_last assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=5
// INTERP: sat_hi assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=5
// INTERP: sat_zero assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c8_i64 = sea.const 8 : i64
  %b2 = sea.icmp ule, %n1, %c8_i64 : i64
  sea.assume %b2
  %c5_i64 = sea.const 5 : i64
  %p3 = sea.ptr.add %A, %c5_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %c1_i64 = sea.const 1 : i64
  %o5 = sea.add %n1, %c1_i64 : i64
  %p6 = sea.ptr.add %B, %o5 : !sea.ptr<64>, i64
  %x7 = sea.load %m0[%p6] : !m -> i8
  %c6_i64 = sea.const 6 : i64
  %o8 = sea.add %n1, %c6_i64 : i64
  %p9 = sea.ptr.add %A, %o8 : !sea.ptr<64>, i64
  %x10 = sea.load %m0[%p9] : !m -> i8
  %c2_i64 = sea.const 2 : i64
  %p11 = sea.ptr.add %B, %c2_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m0[%p11] : !m -> i8
  %p13 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %x14 = sea.load %m0[%p13] : !m -> i8
  %p15 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %p16 = sea.ptr.add %B, %c2_i64 : !sea.ptr<64>, i64
  %m17 = sea.memmove %m0[%p15], %m0[%p16], %n1 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z18 = sea.icmp eq, %n1, %c0_i64 : i64
  %z19 = sea.icmp ne, %n1, %c0_i64 : i64
  %p20 = sea.ptr.add %A, %c5_i64 : !sea.ptr<64>, i64
  %x21 = sea.load %m17[%p20] : !m -> i8
  %e22 = sea.icmp eq, %x21, %x4 : i8
  %o23 = sea.add %n1, %c5_i64 : i64
  %p24 = sea.ptr.add %A, %o23 : !sea.ptr<64>, i64
  %x25 = sea.load %m17[%p24] : !m -> i8
  %e26 = sea.icmp eq, %x25, %x7 : i8
  %o27 = sea.or %z18, %e26 : i1
  %o28 = sea.add %n1, %c6_i64 : i64
  %p29 = sea.ptr.add %A, %o28 : !sea.ptr<64>, i64
  %x30 = sea.load %m17[%p29] : !m -> i8
  %e31 = sea.icmp eq, %x30, %x10 : i8
  %p32 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %x33 = sea.load %m17[%p32] : !m -> i8
  %e34 = sea.icmp eq, %x33, %x12 : i8
  %o35 = sea.or %z18, %e34 : i1
  %p36 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %x37 = sea.load %m17[%p36] : !m -> i8
  %e38 = sea.icmp eq, %x37, %x14 : i8
  %o39 = sea.or %z19, %e38 : i1
  %o40 = sea.and %e22, %o27 : i1
  %o41 = sea.and %o40, %e31 : i1
  %o42 = sea.and %o41, %o35 : i1
  %o43 = sea.and %o42, %o39 : i1
  sea.assert %o43
  sea.halt
}
sea.program @sat_last {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c8_i64 = sea.const 8 : i64
  %b2 = sea.icmp ule, %n1, %c8_i64 : i64
  sea.assume %b2
  %c2_i64 = sea.const 2 : i64
  %o3 = sea.add %n1, %c2_i64 : i64
  %p4 = sea.ptr.add %B, %o3 : !sea.ptr<64>, i64
  %x5 = sea.load %m0[%p4] : !m -> i8
  %c6_i64 = sea.const 6 : i64
  %p6 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %p7 = sea.ptr.add %B, %c2_i64 : !sea.ptr<64>, i64
  %m8 = sea.memmove %m0[%p6], %m0[%p7], %n1 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z9 = sea.icmp eq, %n1, %c0_i64 : i64
  %z10 = sea.icmp ne, %n1, %c0_i64 : i64
  %c5_i64 = sea.const 5 : i64
  %o11 = sea.add %n1, %c5_i64 : i64
  %p12 = sea.ptr.add %A, %o11 : !sea.ptr<64>, i64
  %x13 = sea.load %m8[%p12] : !m -> i8
  %e14 = sea.icmp eq, %x13, %x5 : i8
  %o15 = sea.or %z9, %e14 : i1
  sea.assert %o15
  sea.halt
}
sea.program @sat_hi {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c8_i64 = sea.const 8 : i64
  %b2 = sea.icmp ule, %n1, %c8_i64 : i64
  sea.assume %b2
  %c2_i64 = sea.const 2 : i64
  %o3 = sea.add %n1, %c2_i64 : i64
  %p4 = sea.ptr.add %B, %o3 : !sea.ptr<64>, i64
  %x5 = sea.load %m0[%p4] : !m -> i8
  %c6_i64 = sea.const 6 : i64
  %p6 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %p7 = sea.ptr.add %B, %c2_i64 : !sea.ptr<64>, i64
  %m8 = sea.memmove %m0[%p6], %m0[%p7], %n1 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z9 = sea.icmp eq, %n1, %c0_i64 : i64
  %z10 = sea.icmp ne, %n1, %c0_i64 : i64
  %o11 = sea.add %n1, %c6_i64 : i64
  %p12 = sea.ptr.add %A, %o11 : !sea.ptr<64>, i64
  %x13 = sea.load %m8[%p12] : !m -> i8
  %e14 = sea.icmp eq, %x13, %x5 : i8
  sea.assert %e14
  sea.halt
}
sea.program @sat_zero {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "n" [3] : i64
  %c8_i64 = sea.const 8 : i64
  %b2 = sea.icmp ule, %n1, %c8_i64 : i64
  sea.assume %b2
  %c2_i64 = sea.const 2 : i64
  %p3 = sea.ptr.add %B, %c2_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %c6_i64 = sea.const 6 : i64
  %p5 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %p6 = sea.ptr.add %B, %c2_i64 : !sea.ptr<64>, i64
  %m7 = sea.memmove %m0[%p5], %m0[%p6], %n1 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z8 = sea.icmp eq, %n1, %c0_i64 : i64
  %z9 = sea.icmp ne, %n1, %c0_i64 : i64
  %p10 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %x11 = sea.load %m7[%p10] : !m -> i8
  %e12 = sea.icmp eq, %x11, %x4 : i8
  %o13 = sea.or %z9, %e12 : i1
  sea.assert %o13
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
