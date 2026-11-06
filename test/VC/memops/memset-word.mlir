// memset on word cells: a word cell is 4 copies of the byte; i32 loads of the result.
// Byte memory, i32 loads at 4-aligned offsets, memset A[4..12) <- 0xAB (const) or v (nondet byte:
// the word is zext(v) * 0x01010101). Each program is checked as byte memory (--lower) and with
// --widen-memory=4 (two 4-byte cells). Fixed cell values: unset words keep their old value.
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: unsat unsat
// EXPECT: unsat unsat -- --widen-memory=4
// EXPECT: sat_hi sat
// EXPECT: sat_hi sat -- --widen-memory=4
// EXPECT: sat_lo sat
// EXPECT: sat_lo sat -- --widen-memory=4
// EXPECT: unsat_nd unsat
// EXPECT: unsat_nd unsat -- --widen-memory=4
// EXPECT: sat_nd sat
// EXPECT: sat_nd sat -- --widen-memory=4
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=171
// INTERP: sat_hi assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=171
// INTERP: sat_lo assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=171
// INTERP: unsat_nd halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=171
// INTERP: sat_nd assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=171
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c171_i8 = sea.const 171 : i8
  %c2880154539_i32 = sea.const 2880154539 : i32
  %x1 = sea.load %m0[%A] : !m -> i32
  %c12_i64 = sea.const 12 : i64
  %p2 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i32
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memset %m0[%p4], %c171_i8, %c8_i64 inplace : !m
  %p6 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i32
  %e8 = sea.icmp eq, %x7, %c2880154539_i32 : i32
  %p9 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m5[%p9] : !m -> i32
  %e11 = sea.icmp eq, %x10, %c2880154539_i32 : i32
  %x12 = sea.load %m5[%A] : !m -> i32
  %e13 = sea.icmp eq, %x12, %x1 : i32
  %p14 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x15 = sea.load %m5[%p14] : !m -> i32
  %e16 = sea.icmp eq, %x15, %x3 : i32
  %o17 = sea.and %e8, %e11 : i1
  %o18 = sea.and %o17, %e13 : i1
  %o19 = sea.and %o18, %e16 : i1
  sea.assert %o19
  sea.halt
}
sea.program @sat_hi {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c171_i8 = sea.const 171 : i8
  %c2880154539_i32 = sea.const 2880154539 : i32
  %c4_i64 = sea.const 4 : i64
  %p1 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m2 = sea.memset %m0[%p1], %c171_i8, %c8_i64 inplace : !m
  %c12_i64 = sea.const 12 : i64
  %p3 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p3] : !m -> i32
  %e5 = sea.icmp eq, %x4, %c2880154539_i32 : i32
  sea.assert %e5
  sea.halt
}
sea.program @sat_lo {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c171_i8 = sea.const 171 : i8
  %c2880154539_i32 = sea.const 2880154539 : i32
  %c4_i64 = sea.const 4 : i64
  %p1 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m2 = sea.memset %m0[%p1], %c171_i8, %c8_i64 inplace : !m
  %x3 = sea.load %m2[%A] : !m -> i32
  %e4 = sea.icmp eq, %x3, %c2880154539_i32 : i32
  sea.assert %e4
  sea.halt
}
sea.program @unsat_nd {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %z2 = sea.zext %n1 : i8 to i32
  %c16843009_i32 = sea.const 16843009 : i32
  %o3 = sea.mul %z2, %c16843009_i32 : i32
  %x4 = sea.load %m0[%A] : !m -> i32
  %c12_i64 = sea.const 12 : i64
  %p5 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m0[%p5] : !m -> i32
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m8 = sea.memset %m0[%p7], %n1, %c8_i64 inplace : !m
  %p9 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m8[%p9] : !m -> i32
  %e11 = sea.icmp eq, %x10, %o3 : i32
  %p12 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x13 = sea.load %m8[%p12] : !m -> i32
  %e14 = sea.icmp eq, %x13, %o3 : i32
  %x15 = sea.load %m8[%A] : !m -> i32
  %e16 = sea.icmp eq, %x15, %x4 : i32
  %p17 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x18 = sea.load %m8[%p17] : !m -> i32
  %e19 = sea.icmp eq, %x18, %x6 : i32
  %o20 = sea.and %e11, %e14 : i1
  %o21 = sea.and %o20, %e16 : i1
  %o22 = sea.and %o21, %e19 : i1
  sea.assert %o22
  sea.halt
}
sea.program @sat_nd {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %z2 = sea.zext %n1 : i8 to i32
  %c16843009_i32 = sea.const 16843009 : i32
  %o3 = sea.mul %z2, %c16843009_i32 : i32
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m5 = sea.memset %m0[%p4], %n1, %c8_i64 inplace : !m
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i32
  %e8 = sea.icmp eq, %x7, %o3 : i32
  sea.assert %e8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
