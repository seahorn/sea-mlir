// memset with a symbolic length n <= 12 (inside the expansion bound 16).
// A[4..4+n) <- v. The length is a nondet bounded by an assume, so the expansion is exact. @unsat: the
// last set cell (n > 0) is v and cell 4+n is untouched; @sat_over claims cell 4+n is set, @sat_under that
// cell 4+n-1 keeps its old value. The memset.symbolic-bound warning is expected (an explicit bound).
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: unsat unsat warn:memset.symbolic-bound
// EXPECT-SEAM: unsat unsat
// EXPECT: sat_over sat warn:memset.symbolic-bound
// EXPECT-SEAM: sat_over sat
// EXPECT: sat_under sat warn:memset.symbolic-bound
// EXPECT-SEAM: sat_under sat
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=0 4=200
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=5 4=200
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=12 4=200
// INTERP: sat_over assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=5 4=200
// INTERP: sat_under assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=5 4=200
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [4] : i8
  %n2 = sea.nondet "n" [3] : i64
  %c12_i64 = sea.const 12 : i64
  %b3 = sea.icmp ule, %n2, %c12_i64 : i64
  sea.assume %b3
  %c4_i64 = sea.const 4 : i64
  %o4 = sea.add %n2, %c4_i64 : i64
  %p5 = sea.ptr.add %A, %o4 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %c3_i64 = sea.const 3 : i64
  %o7 = sea.add %n2, %c3_i64 : i64
  %p8 = sea.ptr.add %A, %o7 : !sea.ptr<64>, i64
  %x9 = sea.load %m0[%p8] : !m -> i8
  %p10 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m11 = sea.memset %m0[%p10], %n1, %n2 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z12 = sea.icmp eq, %n2, %c0_i64 : i64
  %o13 = sea.add %n2, %c3_i64 : i64
  %p14 = sea.ptr.add %A, %o13 : !sea.ptr<64>, i64
  %x15 = sea.load %m11[%p14] : !m -> i8
  %e16 = sea.icmp eq, %x15, %n1 : i8
  %o17 = sea.or %z12, %e16 : i1
  %o18 = sea.add %n2, %c4_i64 : i64
  %p19 = sea.ptr.add %A, %o18 : !sea.ptr<64>, i64
  %x20 = sea.load %m11[%p19] : !m -> i8
  %e21 = sea.icmp eq, %x20, %x6 : i8
  %o22 = sea.and %o17, %e21 : i1
  sea.assert %o22
  sea.halt
}
sea.program @sat_over {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [4] : i8
  %n2 = sea.nondet "n" [3] : i64
  %c12_i64 = sea.const 12 : i64
  %b3 = sea.icmp ule, %n2, %c12_i64 : i64
  sea.assume %b3
  %c4_i64 = sea.const 4 : i64
  %o4 = sea.add %n2, %c4_i64 : i64
  %p5 = sea.ptr.add %A, %o4 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m8 = sea.memset %m0[%p7], %n1, %n2 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z9 = sea.icmp eq, %n2, %c0_i64 : i64
  %o10 = sea.add %n2, %c4_i64 : i64
  %p11 = sea.ptr.add %A, %o10 : !sea.ptr<64>, i64
  %x12 = sea.load %m8[%p11] : !m -> i8
  %e13 = sea.icmp eq, %x12, %n1 : i8
  sea.assert %e13
  sea.halt
}
sea.program @sat_under {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [4] : i8
  %n2 = sea.nondet "n" [3] : i64
  %c12_i64 = sea.const 12 : i64
  %b3 = sea.icmp ule, %n2, %c12_i64 : i64
  sea.assume %b3
  %c3_i64 = sea.const 3 : i64
  %o4 = sea.add %n2, %c3_i64 : i64
  %p5 = sea.ptr.add %A, %o4 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m8 = sea.memset %m0[%p7], %n1, %n2 inplace : !m
  %c0_i64 = sea.const 0 : i64
  %z9 = sea.icmp eq, %n2, %c0_i64 : i64
  %o10 = sea.add %n2, %c3_i64 : i64
  %p11 = sea.ptr.add %A, %o10 : !sea.ptr<64>, i64
  %x12 = sea.load %m8[%p11] : !m -> i8
  %e13 = sea.icmp eq, %x12, %x6 : i8
  %o14 = sea.or %z9, %e13 : i1
  sea.assert %o14
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
