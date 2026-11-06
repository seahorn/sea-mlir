// RUN: sea-vc %s --lower --seam | FileCheck %s
// RUN: sea-vc %s --lower --seam --smt2 | FileCheck --check-prefix=SMT %s
// CHECK: {{^}}result: unsat{{$}}
// SMT-NOT: sign_extend
// A negative numeral read back from memory: sext(-3:bv32) must fold to -3:bv64
// (not 2^32-3), so b+8+sext(-3) is exactly b+5.
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @sext_negative_numeral {
^bb0:
  %m0 = sea.mem.nondet "region.0" [0] : !m
  %c4 = sea.const 4 : i64
  %c64 = sea.const 64 : i64
  %cm3 = sea.const -3 : i32
  %c8 = sea.const 8 : i64
  %c5 = sea.const 5 : i64
  %a = sea.alloca %c4 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%a], %cm3 inplace align 4 : !m, i32
  %x = sea.load %m1[%a] align 4 : !m -> i32
  %xw = sea.sext %x : i32 to i64
  %b8 = sea.ptr.add %b, %c8 : !sea.ptr<64>, i64
  %p = sea.ptr.add %b8, %xw : !sea.ptr<64>, i64
  %v = sea.nondet "v" [3] : i8
  %m2 = sea.store %m1[%p], %v inplace align 1 : !m, i8
  %q = sea.ptr.add %b, %c5 : !sea.ptr<64>, i64
  %r = sea.load %m2[%q] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
