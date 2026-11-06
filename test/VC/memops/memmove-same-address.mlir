// memmove with dst = src (a no-op on the cells).
// Cells are nondet bytes; the claims compare a cell of the result with a cell read
// before the op (loads of an earlier memory version are not used: replay-unsound). @unsat claims the
// four boundary reads (d-1 and d+n unchanged; d and d+n-1 copied) and a middle cell;
// @sat_lo / sat_first / sat_last / sat_hi each claim one boundary shifted by one,
// which fails on distinct cells. The interpreter runs use distinct cell values
// (A: 10+i, B: 100+i), so every @sat_* must fail and @unsat must hold.
// A[4..12) <- A[4..12): the sat claims need cells s+1 etc. to differ from the copy.
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: unsat unsat
// EXPECT: sat_lo sat
// EXPECT: sat_first sat
// EXPECT: sat_last sat
// EXPECT: sat_hi sat
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1
// INTERP: sat_lo assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1
// INTERP: sat_first assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1
// INTERP: sat_last assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1
// INTERP: sat_hi assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c3_i64 = sea.const 3 : i64
  %p1 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p3 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m0[%p3] : !m -> i8
  %c11_i64 = sea.const 11 : i64
  %p5 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p7 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m0[%p7] : !m -> i8
  %c8_i64 = sea.const 8 : i64
  %p9 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m0[%p9] : !m -> i8
  %p11 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p12 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m13 = sea.memmove %m0[%p11], %m0[%p12], %c8_i64 inplace : !m
  %p14 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x15 = sea.load %m13[%p14] : !m -> i8
  %e16 = sea.icmp eq, %x15, %x2 : i8
  %p17 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x18 = sea.load %m13[%p17] : !m -> i8
  %e19 = sea.icmp eq, %x18, %x4 : i8
  %p20 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x21 = sea.load %m13[%p20] : !m -> i8
  %e22 = sea.icmp eq, %x21, %x6 : i8
  %p23 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x24 = sea.load %m13[%p23] : !m -> i8
  %e25 = sea.icmp eq, %x24, %x8 : i8
  %p26 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x27 = sea.load %m13[%p26] : !m -> i8
  %e28 = sea.icmp eq, %x27, %x10 : i8
  %o29 = sea.and %e16, %e19 : i1
  %o30 = sea.and %o29, %e22 : i1
  %o31 = sea.and %o30, %e25 : i1
  %o32 = sea.and %o31, %e28 : i1
  sea.assert %o32
  sea.halt
}
sea.program @sat_lo {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c4_i64 = sea.const 4 : i64
  %p1 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %p3 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memmove %m0[%p3], %m0[%p4], %c8_i64 inplace : !m
  %c3_i64 = sea.const 3 : i64
  %p6 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %x2 : i8
  sea.assert %e8
  sea.halt
}
sea.program @sat_first {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c5_i64 = sea.const 5 : i64
  %p1 = sea.ptr.add %A, %c5_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p3 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memmove %m0[%p3], %m0[%p4], %c8_i64 inplace : !m
  %p6 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %x2 : i8
  sea.assert %e8
  sea.halt
}
sea.program @sat_last {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c12_i64 = sea.const 12 : i64
  %p1 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p3 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memmove %m0[%p3], %m0[%p4], %c8_i64 inplace : !m
  %c11_i64 = sea.const 11 : i64
  %p6 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %x2 : i8
  sea.assert %e8
  sea.halt
}
sea.program @sat_hi {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c11_i64 = sea.const 11 : i64
  %p1 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p3 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memmove %m0[%p3], %m0[%p4], %c8_i64 inplace : !m
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %x2 : i8
  sea.assert %e8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
