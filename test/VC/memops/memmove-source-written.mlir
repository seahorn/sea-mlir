// a memmove whose source was written just before (store, store, memset).
// @xobj_*: A[2] <- v, A[9] <- w, then B[5..13) <- A[2..10): B[5] is v (not the old A[2]), B[12] is w.
// @inobj_*: A[6] <- v, then A[8..12) <- A[6..10) (overlap): A[8] = v, A[6] still v, A[9] = old A[7];
// A[10] is old A[8] (not v). @memset_*: memset A[0..8) <- v, then B[0..8) <- A[0..8): B[0..7] = v,
// B[8] untouched. Old values are read before the writes (earlier-version loads are replay-unsound).
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: xobj_unsat unsat
// EXPECT: inobj_unsat unsat
// EXPECT: memset_unsat unsat
// EXPECT: xobj_sat_old sat
// EXPECT: xobj_sat_vw sat
// EXPECT: inobj_sat sat
// EXPECT: memset_sat sat
// INTERP: xobj_unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
// INTERP: inobj_unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
// INTERP: memset_unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
// INTERP: xobj_sat_old assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
// INTERP: xobj_sat_vw assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
// INTERP: inobj_sat assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
// INTERP: memset_sat assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 4=201
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @xobj_unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c2_i64 = sea.const 2 : i64
  %p1 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %n3 = sea.nondet "v" [3] : i8
  %n4 = sea.nondet "w" [4] : i8
  %p5 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %m6 = sea.store %m0[%p5], %n3 inplace : !m, i8
  %c9_i64 = sea.const 9 : i64
  %p7 = sea.ptr.add %A, %c9_i64 : !sea.ptr<64>, i64
  %m8 = sea.store %m6[%p7], %n4 inplace : !m, i8
  %c5_i64 = sea.const 5 : i64
  %p9 = sea.ptr.add %B, %c5_i64 : !sea.ptr<64>, i64
  %p10 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m11 = sea.memmove %m8[%p9], %m8[%p10], %c8_i64 inplace : !m
  %p12 = sea.ptr.add %B, %c5_i64 : !sea.ptr<64>, i64
  %x13 = sea.load %m11[%p12] : !m -> i8
  %e14 = sea.icmp eq, %x13, %n3 : i8
  %c12_i64 = sea.const 12 : i64
  %p15 = sea.ptr.add %B, %c12_i64 : !sea.ptr<64>, i64
  %x16 = sea.load %m11[%p15] : !m -> i8
  %e17 = sea.icmp eq, %x16, %n4 : i8
  %o18 = sea.and %e14, %e17 : i1
  sea.assert %o18
  sea.halt
}
sea.program @xobj_sat_old {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c2_i64 = sea.const 2 : i64
  %p1 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %n3 = sea.nondet "v" [3] : i8
  %n4 = sea.nondet "w" [4] : i8
  %p5 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %m6 = sea.store %m0[%p5], %n3 inplace : !m, i8
  %c9_i64 = sea.const 9 : i64
  %p7 = sea.ptr.add %A, %c9_i64 : !sea.ptr<64>, i64
  %m8 = sea.store %m6[%p7], %n4 inplace : !m, i8
  %c5_i64 = sea.const 5 : i64
  %p9 = sea.ptr.add %B, %c5_i64 : !sea.ptr<64>, i64
  %p10 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m11 = sea.memmove %m8[%p9], %m8[%p10], %c8_i64 inplace : !m
  %p12 = sea.ptr.add %B, %c5_i64 : !sea.ptr<64>, i64
  %x13 = sea.load %m11[%p12] : !m -> i8
  %e14 = sea.icmp eq, %x13, %x2 : i8
  sea.assert %e14
  sea.halt
}
sea.program @xobj_sat_vw {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c2_i64 = sea.const 2 : i64
  %p1 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %x2 = sea.load %m0[%p1] : !m -> i8
  %n3 = sea.nondet "v" [3] : i8
  %n4 = sea.nondet "w" [4] : i8
  %p5 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %m6 = sea.store %m0[%p5], %n3 inplace : !m, i8
  %c9_i64 = sea.const 9 : i64
  %p7 = sea.ptr.add %A, %c9_i64 : !sea.ptr<64>, i64
  %m8 = sea.store %m6[%p7], %n4 inplace : !m, i8
  %c5_i64 = sea.const 5 : i64
  %p9 = sea.ptr.add %B, %c5_i64 : !sea.ptr<64>, i64
  %p10 = sea.ptr.add %A, %c2_i64 : !sea.ptr<64>, i64
  %c8_i64 = sea.const 8 : i64
  %m11 = sea.memmove %m8[%p9], %m8[%p10], %c8_i64 inplace : !m
  %p12 = sea.ptr.add %B, %c5_i64 : !sea.ptr<64>, i64
  %x13 = sea.load %m11[%p12] : !m -> i8
  %e14 = sea.icmp eq, %x13, %n4 : i8
  sea.assert %e14
  sea.halt
}
sea.program @inobj_unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c8_i64 = sea.const 8 : i64
  %p2 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %c10_i64 = sea.const 10 : i64
  %p4 = sea.ptr.add %A, %c10_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m0[%p4] : !m -> i8
  %c6_i64 = sea.const 6 : i64
  %p6 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %m7 = sea.store %m0[%p6], %n1 inplace : !m, i8
  %p8 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %p9 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %c4_i64 = sea.const 4 : i64
  %m10 = sea.memmove %m7[%p8], %m7[%p9], %c4_i64 inplace : !m
  %c7_i64 = sea.const 7 : i64
  %p11 = sea.ptr.add %A, %c7_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m0[%p11] : !m -> i8
  %p13 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x14 = sea.load %m10[%p13] : !m -> i8
  %e15 = sea.icmp eq, %x14, %n1 : i8
  %p16 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %x17 = sea.load %m10[%p16] : !m -> i8
  %e18 = sea.icmp eq, %x17, %n1 : i8
  %c9_i64 = sea.const 9 : i64
  %p19 = sea.ptr.add %A, %c9_i64 : !sea.ptr<64>, i64
  %x20 = sea.load %m10[%p19] : !m -> i8
  %e21 = sea.icmp eq, %x20, %x12 : i8
  %o22 = sea.and %e15, %e18 : i1
  %o23 = sea.and %o22, %e21 : i1
  sea.assert %o23
  sea.halt
}
sea.program @inobj_sat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c8_i64 = sea.const 8 : i64
  %p2 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %c10_i64 = sea.const 10 : i64
  %p4 = sea.ptr.add %A, %c10_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m0[%p4] : !m -> i8
  %c6_i64 = sea.const 6 : i64
  %p6 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %m7 = sea.store %m0[%p6], %n1 inplace : !m, i8
  %p8 = sea.ptr.add %A, %c8_i64 : !sea.ptr<64>, i64
  %p9 = sea.ptr.add %A, %c6_i64 : !sea.ptr<64>, i64
  %c4_i64 = sea.const 4 : i64
  %m10 = sea.memmove %m7[%p8], %m7[%p9], %c4_i64 inplace : !m
  %c7_i64 = sea.const 7 : i64
  %p11 = sea.ptr.add %A, %c7_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m0[%p11] : !m -> i8
  %p13 = sea.ptr.add %A, %c10_i64 : !sea.ptr<64>, i64
  %x14 = sea.load %m10[%p13] : !m -> i8
  %e15 = sea.icmp eq, %x14, %n1 : i8
  sea.assert %e15
  sea.halt
}
sea.program @memset_unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c8_i64 = sea.const 8 : i64
  %p2 = sea.ptr.add %B, %c8_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %m4 = sea.memset %m0[%A], %n1, %c8_i64 inplace : !m
  %m5 = sea.memmove %m4[%B], %m4[%A], %c8_i64 inplace : !m
  %x6 = sea.load %m5[%B] : !m -> i8
  %e7 = sea.icmp eq, %x6, %n1 : i8
  %c7_i64 = sea.const 7 : i64
  %p8 = sea.ptr.add %B, %c7_i64 : !sea.ptr<64>, i64
  %x9 = sea.load %m5[%p8] : !m -> i8
  %e10 = sea.icmp eq, %x9, %n1 : i8
  %p11 = sea.ptr.add %B, %c8_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m5[%p11] : !m -> i8
  %e13 = sea.icmp eq, %x12, %x3 : i8
  %o14 = sea.and %e7, %e10 : i1
  %o15 = sea.and %o14, %e13 : i1
  sea.assert %o15
  sea.halt
}
sea.program @memset_sat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %n1 = sea.nondet "v" [3] : i8
  %c8_i64 = sea.const 8 : i64
  %p2 = sea.ptr.add %B, %c8_i64 : !sea.ptr<64>, i64
  %x3 = sea.load %m0[%p2] : !m -> i8
  %m4 = sea.memset %m0[%A], %n1, %c8_i64 inplace : !m
  %m5 = sea.memmove %m4[%B], %m4[%A], %c8_i64 inplace : !m
  %p6 = sea.ptr.add %B, %c8_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m5[%p6] : !m -> i8
  %e8 = sea.icmp eq, %x7, %n1 : i8
  sea.assert %e8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
