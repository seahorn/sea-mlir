// memset / memmove / memhavoc with length 0 are no-ops.
// Constant length 0 at A+4. @*_unsat: the cell at A+4 keeps its value. @*_sat: it claims the cell took the
// memset value / the source cell (memmove, memhavoc), which would need a non-empty range.
// RUN: %python %S/Inputs/run-memops.py %s %sea_vc
// EXPECT: memset_unsat unsat
// EXPECT: memset_sat sat
// EXPECT: memmove_unsat unsat
// EXPECT: memmove_sat sat
// EXPECT: memhavoc_unsat unsat
// EXPECT: memhavoc_sat sat
// INTERP: memset_unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 5@heap.1+*=200+1
// INTERP: memset_sat assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 5@heap.1+*=200+1
// INTERP: memmove_unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 5@heap.1+*=200+1
// INTERP: memmove_sat assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 5@heap.1+*=200+1
// INTERP: memhavoc_unsat halted 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 5@heap.1+*=200+1
// INTERP: memhavoc_sat assert-failed 0@heap.1+*=10+1 0@stack.2+*=100+1 3=200 5@heap.1+*=200+1
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @memset_unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p4] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p6] : !m -> i8
  %c0_i64 = sea.const 0 : i64
  %p8 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m9 = sea.memset %m2[%p8], %n3, %c0_i64 inplace : !m
  %p10 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x11 = sea.load %m9[%p10] : !m -> i8
  %e12 = sea.icmp eq, %x11, %x5 : i8
  sea.assert %e12
  sea.halt
}
sea.program @memset_sat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p4] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p6] : !m -> i8
  %c0_i64 = sea.const 0 : i64
  %p8 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m9 = sea.memset %m2[%p8], %n3, %c0_i64 inplace : !m
  %p10 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x11 = sea.load %m9[%p10] : !m -> i8
  %e12 = sea.icmp eq, %x11, %n3 : i8
  sea.assert %e12
  sea.halt
}
sea.program @memmove_unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p4] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p6] : !m -> i8
  %c0_i64 = sea.const 0 : i64
  %p8 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p9 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %m10 = sea.memmove %m2[%p8], %m2[%p9], %c0_i64 inplace : !m
  %p11 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m10[%p11] : !m -> i8
  %e13 = sea.icmp eq, %x12, %x5 : i8
  sea.assert %e13
  sea.halt
}
sea.program @memmove_sat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p4] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p6] : !m -> i8
  %c0_i64 = sea.const 0 : i64
  %p8 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %p9 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %m10 = sea.memmove %m2[%p8], %m2[%p9], %c0_i64 inplace : !m
  %p11 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x12 = sea.load %m10[%p11] : !m -> i8
  %e13 = sea.icmp eq, %x12, %x7 : i8
  sea.assert %e13
  sea.halt
}
sea.program @memhavoc_unsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p4] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p6] : !m -> i8
  %c0_i64 = sea.const 0 : i64
  %p8 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m9 = sea.memhavoc %m2[%p8], %c0_i64 [5] inplace : !m
  %p10 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x11 = sea.load %m9[%p10] : !m -> i8
  %e12 = sea.icmp eq, %x11, %x5 : i8
  sea.assert %e12
  sea.halt
}
sea.program @memhavoc_sat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32_i64 = sea.const 32 : i64
  %A = sea.malloc %c32_i64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c32_i64 align 8 [2] : i64 -> !sea.ptr<64>
  %c30_i64 = sea.const 30 : i64
  %p1 = sea.ptr.add %A, %c30_i64 : !sea.ptr<64>, i64
  %c1_i8 = sea.const 1 : i8
  %m2 = sea.store %m0[%p1], %c1_i8 inplace : !m, i8
  %n3 = sea.nondet "v" [3] : i8
  %c4_i64 = sea.const 4 : i64
  %p4 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p4] : !m -> i8
  %c12_i64 = sea.const 12 : i64
  %p6 = sea.ptr.add %A, %c12_i64 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p6] : !m -> i8
  %c0_i64 = sea.const 0 : i64
  %p8 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %m9 = sea.memhavoc %m2[%p8], %c0_i64 [5] inplace : !m
  %p10 = sea.ptr.add %A, %c4_i64 : !sea.ptr<64>, i64
  %x11 = sea.load %m9[%p10] : !m -> i8
  %e12 = sea.icmp eq, %x11, %x7 : i8
  sea.assert %e12
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
