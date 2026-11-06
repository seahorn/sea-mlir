// RUN: sea-vc %s --lower --seam | FileCheck %s
// RUN: sea-vc %s --lower --seam --smt2 | FileCheck --check-prefix=SMT %s
// CHECK: {{^}}result: unsat{{$}}
// SMT-NOT: zero_extend
// SeaM: an index read back from memory resolves to a numeral (3), but its
// widening zext(3:bv32) is not folded, so b+3 vs b+zext(3) stays open.
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @zext_numeral {
^bb0:
  %m0 = sea.mem.nondet "region.0" [0] : !m
  %c4 = sea.const 4 : i64
  %c64 = sea.const 64 : i64
  %c3 = sea.const 3 : i32
  %c3w = sea.const 3 : i64
  %a = sea.alloca %c4 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%a], %c3 inplace align 4 : !m, i32
  %x = sea.load %m1[%a] align 4 : !m -> i32
  %xw = sea.zext %x : i32 to i64
  %p = sea.ptr.add %b, %xw : !sea.ptr<64>, i64
  %v = sea.nondet "v" [3] : i8
  %m2 = sea.store %m1[%p], %v inplace align 1 : !m, i8
  %q = sea.ptr.add %b, %c3w : !sea.ptr<64>, i64
  %r = sea.load %m2[%q] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
