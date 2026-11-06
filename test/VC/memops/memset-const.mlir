// memset with a constant length, boundary reads (d-1, d, d+n-1, d+n).
// A[4..12) <- v (nondet byte). @unsat: d-1 and d+n keep their old value, d, d+n-1 and a middle
// cell are v. @sat_*: one boundary claimed wrongly. Interpreter inputs: distinct cells, v=200.
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: unsat unsat
// EXPECT: sat_lo sat
// EXPECT: sat_hi sat
// EXPECT: sat_first sat
// EXPECT: sat_last sat
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200
// INTERP: sat_lo assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200
// INTERP: sat_hi assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200
// INTERP: sat_first assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200
// INTERP: sat_last assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c3_i64 = sea.const 3 : i64
  %p2 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p4 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m0[%p4] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p6 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m7 = sea.memset %m0[%p6], %n1, %c8_i64 inplace : !m
  %p8 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x9 = sea.load %m7[%p8] : !m -> i8
  %e10 = sea.icmp eq, %x9, %x3 : i8
  %p11 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m7[%p11] : !m -> i8
  %e13 = sea.icmp eq, %x12, %x5 : i8
  %p14 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x15 = sea.load %m7[%p14] : !m -> i8
  %e16 = sea.icmp eq, %x15, %n1 : i8
  %c11_i64 = sea.const 11 : i64
  %p17 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x18 = sea.load %m7[%p17] : !m -> i8
  %e19 = sea.icmp eq, %x18, %n1 : i8
  %c10_i64 = sea.const 10 : i64
  %p20 = sea.ptr.add %A, %c10_i64 : !sea.ptr<64>, i64
  %x21 = sea.load %m7[%p20] : !m -> i8
  %e22 = sea.icmp eq, %x21, %n1 : i8
  %o23 = sea.and %e10, %e13 : i1
  %o24 = sea.and %o23, %e16 : i1
  %o25 = sea.and %o24, %e19 : i1
  %o26 = sea.and %o25, %e22 : i1
  sea.assert %o26
  sea.halt
}
sea.program @sat_lo {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p2 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m3 = sea.memset %m0[%p2], %n1, %c8_i64 inplace : !m
  %c3_i64 = sea.const 3 : i64
  %p4 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m3[%p4] : !m -> i8
  %e6 = sea.icmp eq, %x5, %n1 : i8
  sea.assert %e6
  sea.halt
}
sea.program @sat_hi {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p2 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m3 = sea.memset %m0[%p2], %n1, %c8_i64 inplace : !m
  %c12_i64 = sea.const 12 : i64
  %p4 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m3[%p4] : !m -> i8
  %e6 = sea.icmp eq, %x5, %n1 : i8
  sea.assert %e6
  sea.halt
}
sea.program @sat_first {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p2 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memset %m0[%p4], %n1, %c8_i64 inplace : !m
  %p6 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %x3 : i8
  sea.assert %e8
  sea.halt
}
sea.program @sat_last {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c11_i64 = sea.const 11 : i64
  %p2 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memset %m0[%p4], %n1, %c8_i64 inplace : !m
  %p6 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %x3 : i8
  sea.assert %e8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
