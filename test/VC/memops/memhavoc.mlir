// memhavoc with a constant length: boundaries, havocked cells are stable and copyable; symbolic length refused.
// A[4..12) havocked (site 5: the havoc array H; cell k of the range reads H[4+k]). The object is written before
// (a memhavoc of an untouched object is dropped by sea-drop-fresh-memhavoc). @unsat: cells d-1 and d+n keep
// their old value, a havocked cell reads the same every time, and a memmove of the havocked range to B
// copies those very values. @sat_first / @sat_last: a havocked cell may differ from its old value.
// @symbolic_len: plain encoding refuses a nondet length (vc.mem-op-symbolic-length).
// The MHAVOC term preserves cell d-1 for any length: unsat.
// --seam: MHAVOC takes the symbolic length (EXPECT-SEAM unsat); plain still refuses it.
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: unsat unsat
// EXPECT: unsat_edges unsat
// EXPECT: sat_first sat
// EXPECT: sat_last sat
// EXPECT: symbolic_len error:vc.mem-op-symbolic-length
// EXPECT-SEAM: symbolic_len unsat
// INTERP: unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 5@heap.1+*=200+1
// INTERP: unsat_edges halted 0@heap.1+*=10+1 0@stack.2+*=100+1 5@heap.1+*=200+1
// INTERP: sat_first assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 5@heap.1+*=200+1
// INTERP: sat_last assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 5@heap.1+*=200+1
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %c3_i64 = sea.const 3 : i64
  %p3 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p3] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p5 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m2[%p5] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m8 = sea.memhavoc %m2[%p7], %c8_i64 [5] inplace : !m
  %p9 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m8[%p9] : !m -> i8
  %c11_i64 = sea.const 11 : i64
  %p11 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m8[%p11] : !m -> i8
  %p13 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m14 = sea.memmove %m8[%B], %m8[%p13], %c8_i64 inplace : !m
  %p15 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x16 = sea.load %m14[%p15] : !m -> i8
  %e17 = sea.icmp eq, %x16, %x4 : i8
  %p18 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x19 = sea.load %m14[%p18] : !m -> i8
  %e20 = sea.icmp eq, %x19, %x6 : i8
  %p21 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x22 = sea.load %m14[%p21] : !m -> i8
  %e23 = sea.icmp eq, %x22, %x10 : i8
  %p24 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x25 = sea.load %m14[%p24] : !m -> i8
  %e26 = sea.icmp eq, %x25, %x12 : i8
  %x27 = sea.load %m14[%B] : !m -> i8
  %e28 = sea.icmp eq, %x27, %x10 : i8
  %c7_i64 = sea.const 7 : i64
  %p29 = sea.ptr.add %B, %c7_i64 : !sea.ptr<64>, i64
  %x30 = sea.load %m14[%p29] : !m -> i8
  %e31 = sea.icmp eq, %x30, %x12 : i8
  %o32 = sea.and %e17, %e20 : i1
  %o33 = sea.and %o32, %e23 : i1
  %o34 = sea.and %o33, %e26 : i1
  %o35 = sea.and %o34, %e28 : i1
  %o36 = sea.and %o35, %e31 : i1
  sea.assert %o36
  sea.halt
}
sea.program @unsat_edges {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %c3_i64 = sea.const 3 : i64
  %p3 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p3] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p5 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x6 = sea.load %m2[%p5] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m8 = sea.memhavoc %m2[%p7], %c8_i64 [5] inplace : !m
  %p9 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m8[%p9] : !m -> i8
  %e11 = sea.icmp eq, %x10, %x4 : i8
  %p12 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x13 = sea.load %m8[%p12] : !m -> i8
  %e14 = sea.icmp eq, %x13, %x6 : i8
  %o15 = sea.and %e11, %e14 : i1
  sea.assert %o15
  sea.halt
}
sea.program @sat_first {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %c4_i64 = sea.const 4 : i64
  %p3 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p3] : !m -> i8
  %p5 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m6 = sea.memhavoc %m2[%p5], %c8_i64 [5] inplace : !m
  %p7 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m6[%p7] : !m -> i8
  %e9 = sea.icmp eq, %x8, %x4 : i8
  sea.assert %e9
  sea.halt
}
sea.program @sat_last {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %c11_i64 = sea.const 11 : i64
  %p3 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p3] : !m -> i8
  %c4_i64 = sea.const 4 : i64
  %p5 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m6 = sea.memhavoc %m2[%p5], %c8_i64 [5] inplace : !m
  %p7 = sea.ptr.add %A, %c11_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m6[%p7] : !m -> i8
  %e9 = sea.icmp eq, %x8, %x4 : i8
  sea.assert %e9
  sea.halt
}
sea.program @symbolic_len {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "n" [3] : i64
  %c8_i64 = sea.const 8 : i64
  %b4 = sea.icmp ule, %n3, %c8_i64 : i64
  sea.assume %b4
  %c4_i64 = sea.const 4 : i64
  %p5 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m6 = sea.memhavoc %m2[%p5], %n3 [5] inplace : !m
  %c3_i64 = sea.const 3 : i64
  %p7 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x8 = sea.load %m6[%p7] : !m -> i8
  %p9 = sea.ptr.add %A, %c3_i64 : !sea.ptr<64>, i64
  %x10 = sea.load %m2[%p9] : !m -> i8
  %e11 = sea.icmp eq, %x8, %x10 : i8
  sea.assert %e11
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
