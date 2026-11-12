// Exact numeral difference: a memset at B of n = ite(c, 0, 16) bytes, read at B.
// The recorded in-test has the numeral difference 0: one base (same-object).
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s
// CHECK: seam.alias.open-memop.same-object: 1
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c32 = sea.const 32 : i64
  %c0 = sea.const 0 : i64
  %c16 = sea.const 16 : i64
  %B = sea.malloc %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [4] : i1
  %n = sea.select %c, %c0, %c16 : i64
  %r = sea.load %m0[%B] : !m -> i8
  %z = sea.const 0 : i8
  %m1 = sea.memset %m0[%B], %z, %n inplace : !m
  %l = sea.load %m1[%B] : !m -> i8
  %ok = sea.icmp eq, %l, %r : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
